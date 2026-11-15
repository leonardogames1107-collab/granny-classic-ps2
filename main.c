#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <libpad.h>
#include <stdio.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <audsrv.h>
#include <string.h>

static unsigned char padBuf[256] __attribute__((aligned(64)));

#define MENU_COUNT 4

/* Zona de ativação do analógico (centro = 0x80). */
#define STICK_LOW  0x40
#define STICK_HIGH 0xC0

/* Posição e tamanho de cada botão na imagem base (640x448). */
typedef struct { float x, y; } ButtonPos;

static const ButtonPos buttonPos[MENU_COUNT] = {
    {64.0f, 117.0f},   /* JOGAR       */
    {64.0f, 193.0f},   /* OPÇÕES      */
    {64.0f, 262.0f},   /* MULTIPLAYER */
    {65.0f, 330.0f}    /* SAIR        */
};

/* Textura válida: carregou e cabe nos limites da GS. */
static int tex_ok(const GSTEXTURE *t)
{
    return t->Width > 0 && t->Height > 0 &&
           t->Width <= 1024 && t->Height <= 1024;
}

/* Efeito sonoro do cursor, misturado por software na música. */
static short sfxBuf[22050 * 2];   /* até 1 s estéreo a 22050 Hz */
static int sfxBytes = 0;
static int sfxPos = -1;           /* posição em amostras; -1 = parado */

static void mix_sfx(char *chunk, int bytes)
{
    short *dst = (short *)chunk;
    int n = bytes / 2;
    int total = sfxBytes / 2;
    int i;

    if (sfxPos < 0 || total <= 0)
        return;

    for (i = 0; i < n && sfxPos < total; i++, sfxPos++)
    {
        int v = dst[i] + sfxBuf[sfxPos];

        if (v > 32767)  v = 32767;
        if (v < -32768) v = -32768;

        dst[i] = (short)v;
    }

    if (sfxPos >= total)
        sfxPos = -1;
}

int main(int argc, char *argv[])
{
    GSGLOBAL *gsGlobal;
    GSTEXTURE baseMenu = {0};
    GSTEXTURE highlights[MENU_COUNT] = {{0}};

    const char *highlightPaths[MENU_COUNT] = {
        "mass:/GRANNY/destaque_jogar.png",
        "mass:/GRANNY/destaque_opcoes.png",
        "mass:/GRANNY/destaque_multiplayer.png",
        "mass:/GRANNY/destaque_sair.png"
    };

    int baseLoaded = 0;
    int highlightLoaded[MENU_COUNT] = {0, 0, 0, 0};
    int selected = 0;
    int padOpened = 0;
    int state;
    int audioReady = 0;

    struct audsrv_fmt_t audioFormat;
    FILE *musicFile = NULL;
    char audioChunk[2048] __attribute__((aligned(16)));
    long musicDataStart = 0;
    int audioPendingSize = 0;

    struct padButtonStatus buttons;
    unsigned short oldButtons = 0;
    unsigned short currentButtons;
    unsigned short pressedButtons;

    int analogSet = 0;
    int oldStickUp = 0;
    int oldStickDown = 0;

    u64 black = GS_SETREG_RGBAQ(5, 5, 8, 0, 0);
    u64 errorRed = GS_SETREG_RGBAQ(0xC0, 0x00, 0x00, 0x80, 0);

    /* RPC antes de qualquer acesso a arquivo ou módulo. */
    sceSifInitRpc(0);

    gsGlobal = gsKit_init_global();

    gsGlobal->PSM = GS_PSM_CT24;
    gsGlobal->PSMZ = GS_PSMZ_16S;
    gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;
    gsGlobal->ZBuffering = GS_SETTING_OFF;

    dmaKit_init(
        D_CTRL_RELE_OFF,
        D_CTRL_MFD_OFF,
        D_CTRL_STS_UNSPEC,
        D_CTRL_STD_OFF,
        D_CTRL_RCYC_8,
        1 << DMA_CHANNEL_GIF
    );

    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsKit_init_screen(gsGlobal);
    gsKit_mode_switch(gsGlobal, GS_ONESHOT);

    /* Fundo único 640x448 (PNG indexado de 8 bits). */
    baseMenu.Delayed = GS_SETTING_ON;
    if (gsKit_texture_png(
            gsGlobal, &baseMenu, "mass:/GRANNY/menu_base.png") == 0 &&
        tex_ok(&baseMenu))
    {
        baseLoaded = 1;
    }
    else
    {
        printf("ERRO: menu_base.png nao carregou\n");
    }

    /* Destaques pequenos, um por botão. */
    for (int i = 0; i < MENU_COUNT; i++)
    {
        highlights[i].Delayed = GS_SETTING_ON;

        if (gsKit_texture_png(
                gsGlobal, &highlights[i], (char *)highlightPaths[i]) == 0 &&
            tex_ok(&highlights[i]))
        {
            highlightLoaded[i] = 1;
        }
        else
        {
            printf("ERRO: %s nao carregou\n", highlightPaths[i]);
        }
    }

    if (SifLoadModule("rom0:LIBSD", 0, NULL) >= 0 &&
        SifLoadModule("mass:/GRANNY/audsrv.irx", 0, NULL) >= 0)
    {
        if (audsrv_init() == 0)
        {
            audioFormat.bits = 16;
            audioFormat.freq = 22050;
            audioFormat.channels = 2;

            if (audsrv_set_format(&audioFormat) == 0)
            {
                audsrv_set_volume(MAX_VOLUME);
                audioReady = 1;

                musicFile = fopen("mass:/GRANNY/musica_menu.wav", "rb");

                if (musicFile != NULL)
                {
                    musicDataStart = 44;
                    fseek(musicFile, musicDataStart, SEEK_SET);
                }
                else
                {
                    audioReady = 0;
                }
            }
        }
    }

    if (audioReady)
    {
        FILE *sfx = fopen("mass:/GRANNY/som_cursor.wav", "rb");

        if (sfx != NULL)
        {
            fseek(sfx, 44, SEEK_SET);
            sfxBytes = (int)fread(sfxBuf, 1, sizeof(sfxBuf), sfx);
            fclose(sfx);
            sfxBytes &= ~3;   /* múltiplo de um quadro estéreo */
        }
    }

    if (padInit(0) == 1)
    {
        if (padPortOpen(0, 0, padBuf) != 0)
            padOpened = 1;
    }

    while (1)
    {
        /* ---------- Áudio ---------- */
        if (audioReady && musicFile != NULL)
        {
            if (audioPendingSize == 0 &&
                audsrv_available() >= (int)sizeof(audioChunk))
            {
                audioPendingSize = fread(
                    audioChunk, 1, sizeof(audioChunk), musicFile);

                if (audioPendingSize == 0)
                {
                    fseek(musicFile, musicDataStart, SEEK_SET);

                    audioPendingSize = fread(
                        audioChunk, 1, sizeof(audioChunk), musicFile);
                }

                if (audioPendingSize > 0)
                    mix_sfx(audioChunk, audioPendingSize);
            }

            if (audioPendingSize > 0 &&
                audsrv_available() >= audioPendingSize)
            {
                int sent = audsrv_play_audio(audioChunk, audioPendingSize);

                if (sent > 0)
                {
                    if (sent < audioPendingSize)
                    {
                        memmove(audioChunk,
                                audioChunk + sent,
                                audioPendingSize - sent);
                    }

                    audioPendingSize -= sent;
                }
            }
        }

        /* ---------- Controle ---------- */
        if (padOpened)
        {
            state = padGetState(0, 0);

            if (state == PAD_STATE_STABLE)
            {
                /* Liga o modo analógico uma única vez e trava. */
                if (!analogSet)
                {
                    padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
                    analogSet = 1;
                }

                if (padRead(0, 0, &buttons) != 0)
                {
                    currentButtons = (unsigned short)(~buttons.btns);
                    pressedButtons = currentButtons & ~oldButtons;
                    oldButtons = currentButtons;

                    int stickUp = 0, stickDown = 0;

                    /* modo 7 = analógico ativo (só o analógico esquerdo) */
                    if ((buttons.mode >> 4) == 0x07)
                    {
                        stickUp   = buttons.ljoy_v < STICK_LOW;
                        stickDown = buttons.ljoy_v > STICK_HIGH;
                    }

                    /* Um movimento por empurrão, igual aos direcionais. */
                    int goUp   = (pressedButtons & PAD_UP) ||
                                 (stickUp && !oldStickUp);
                    int goDown = (pressedButtons & PAD_DOWN) ||
                                 (stickDown && !oldStickDown);

                    oldStickUp = stickUp;
                    oldStickDown = stickDown;

                    if (goUp)
                    {
                        selected = (selected + MENU_COUNT - 1) % MENU_COUNT;
                        if (sfxBytes > 0) sfxPos = 0;
                    }
                    else if (goDown)
                    {
                        selected = (selected + 1) % MENU_COUNT;
                        if (sfxBytes > 0) sfxPos = 0;
                    }

                    if ((pressedButtons & PAD_CROSS) && selected == 3)
                    {
                        if (musicFile != NULL)
                        {
                            fclose(musicFile);
                            musicFile = NULL;
                        }

                        if (audioReady)
                        {
                            audsrv_quit();
                            audioReady = 0;
                        }

                        padPortClose(0, 0);
                        padEnd();
                        return 0;
                    }
                }
            }
        }

        /* ---------- Vídeo ---------- */
        gsKit_clear(gsGlobal, black);

        if (baseLoaded)
        {
            /* Fatores de escala: as posições foram medidas em 640x448,
               mas o modo de vídeo pode ser PAL (640x512). */
            float sx = (float)gsGlobal->Width  / 640.0f;
            float sy = (float)gsGlobal->Height / 448.0f;

            gsKit_TexManager_bind(gsGlobal, &baseMenu);
            gsKit_prim_sprite_texture(
                gsGlobal,
                &baseMenu,
                0.0f, 0.0f,
                0.0f, 0.0f,
                (float)gsGlobal->Width,
                (float)gsGlobal->Height,
                (float)baseMenu.Width,
                (float)baseMenu.Height,
                1,
                GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0)
            );

            /* Destaque opaco só sobre o botão selecionado. */
            if (highlightLoaded[selected])
            {
                float x1 = buttonPos[selected].x * sx;
                float y1 = buttonPos[selected].y * sy;
                float x2 = x1 + (float)highlights[selected].Width  * sx;
                float y2 = y1 + (float)highlights[selected].Height * sy;

                gsKit_TexManager_bind(gsGlobal, &highlights[selected]);
                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &highlights[selected],
                    x1, y1,
                    0.0f, 0.0f,
                    x2, y2,
                    (float)highlights[selected].Width,
                    (float)highlights[selected].Height,
                    2,
                    GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0)
                );
            }
        }
        else
        {
            /* Sem fundo: mostra um quadrado vermelho em vez de congelar. */
            gsKit_prim_sprite(
                gsGlobal, 40.0f, 40.0f, 200.0f, 140.0f, 1, errorRed);
        }

        gsKit_sync_flip(gsGlobal);
        gsKit_queue_exec(gsGlobal);
    }

    return 0;
}
