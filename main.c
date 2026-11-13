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

int main(int argc, char *argv[])
{
    GSGLOBAL *gsGlobal;
    GSFONTM *font;
    GSTEXTURE menus[MENU_COUNT] = {{0}};
    GSTEXTURE fallback = {0};

    const char *paths[MENU_COUNT] = {
        "mass:/GRANNY/menu_jogar_selecionado.png",
        "mass:/GRANNY/menu_opcoes_selecionado.png",
        "mass:/GRANNY/menu_multiplayer_selecionado.png",
        "mass:/GRANNY/menu_sair_selecionado.png"
    };

    int loaded[MENU_COUNT] = {0, 0, 0, 0};
    int fallbackLoaded = 0;
    int selected = 0;
    int padOpened = 0;
    int state;
    int audioReady = 0;
    int oldAnalogDirection = 0;

    struct audsrv_fmt_t audioFormat;
    FILE *musicFile = NULL;
    char audioChunk[2048];
    long musicDataStart = 0;
    int audioPendingSize = 0;

    struct padButtonStatus buttons;
    unsigned short oldButtons = 0;
    unsigned short currentButtons;
    unsigned short pressedButtons;

    u64 black = GS_SETREG_RGBAQ(5, 5, 8, 0, 0);

    gsGlobal = gsKit_init_global();
    font = gsKit_init_fontm();

    gsGlobal->PSM = GS_PSM_CT24;
    gsGlobal->PSMZ = GS_PSMZ_16S;
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
    gsKit_fontm_upload(gsGlobal, font);
    gsKit_mode_switch(gsGlobal, GS_ONESHOT);

    /*
     * Cada imagem já contém o menu completo, com apenas uma placa
     * vermelha. Assim, basta trocar a textura conforme a opção escolhida.
     */
    for (int i = 0; i < MENU_COUNT; i++)
    {
        menus[i].Delayed = GS_SETTING_ON;

        if (gsKit_texture_png(gsGlobal, &menus[i], paths[i]) == 0)
            loaded[i] = 1;
    }

    fallback.Delayed = GS_SETTING_ON;

    if (gsKit_texture_png(
        gsGlobal, &fallback, "mass:/GRANNY/menu_ps2.png"
    ) == 0)
    {
        fallbackLoaded = 1;
    }

    sceSifInitRpc(0);

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

                musicFile = fopen(
                    "mass:/GRANNY/musica_menu.wav", "rb"
                );

                if (musicFile != NULL)
                {
                    /* WAV PCM padrão com cabeçalho de 44 bytes. */
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

    if (padInit(0) == 1)
    {
        if (padPortOpen(0, 0, padBuf) != 0)
            padOpened = 1;
    }

    while (1)
    {
        /* Música de fundo em loop. */
        if (audioReady && musicFile != NULL)
        {
            if (audioPendingSize == 0 &&
                audsrv_available() >= (int)sizeof(audioChunk))
            {
                audioPendingSize = fread(
                    audioChunk, 1, sizeof(audioChunk), musicFile
                );

                if (audioPendingSize == 0)
                {
                    fseek(musicFile, musicDataStart, SEEK_SET);

                    audioPendingSize = fread(
                        audioChunk, 1, sizeof(audioChunk), musicFile
                    );
                }
            }

            if (audioPendingSize > 0 &&
                audsrv_available() >= audioPendingSize)
            {
                int sent = audsrv_play_audio(
                    audioChunk, audioPendingSize
                );

                if (sent > 0)
                {
                    if (sent < audioPendingSize)
                    {
                        memmove(
                            audioChunk,
                            audioChunk + sent,
                            audioPendingSize - sent
                        );
                    }

                    audioPendingSize -= sent;
                }
            }
        }

        /* Navegação pelo controle. */
        if (padOpened)
        {
            state = padGetState(0, 0);

            if (state == PAD_STATE_STABLE)
            {
                if (padRead(0, 0, &buttons) != 0)
                {
                    currentButtons = (unsigned short)(~buttons.btns);
                    pressedButtons = currentButtons & ~oldButtons;
                    oldButtons = currentButtons;
                    
if (pressedButtons & PAD_UP)
{
    selected = (selected + MENU_COUNT - 1) % MENU_COUNT;
    oldAnalogDirection = 0;
}
else if (pressedButtons & PAD_DOWN)
{
    selected = (selected + 1) % MENU_COUNT;
    oldAnalogDirection = 0;
}
else if (buttons.ljoy_v < 80)
{
    if (oldAnalogDirection != -1)
        selected = (selected + MENU_COUNT - 1) % MENU_COUNT;

    oldAnalogDirection = -1;
}
else if (buttons.ljoy_v > 175)
{
    if (oldAnalogDirection != 1)
        selected = (selected + 1) % MENU_COUNT;

    oldAnalogDirection = 1;
}
else
{
    oldAnalogDirection = 0;
}
                    
                        selected = (selected + 1) % MENU_COUNT;

                    /* Analógico desativado temporariamente para teste. */
oldAnalogDirection = 0;
                    }

                    /* Sair é a quarta opção. */
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

                    /*
                     * JOGAR, OPÇÕES e MULTIPLAYER já aparecem e podem
                     * ser selecionados. Suas telas/funções serão ligadas
                     * nas próximas etapas do projeto.
                     */
                }
            }
        }

        gsKit_clear(gsGlobal, black);

        /*
         * Exibe o menu preservando a proporção original.
         */
        GSTEXTURE *currentMenu = loaded[selected]
            ? &menus[selected]
            : &fallback;

        if (loaded[selected] || fallbackLoaded)
        {
            float imageWidth = (float)currentMenu->Width;
            float imageHeight = (float)currentMenu->Height;

            float scaleX = (float)gsGlobal->Width / imageWidth;
            float scaleY = (float)gsGlobal->Height / imageHeight;

            float scale = scaleX < scaleY ? scaleX : scaleY;

            float drawWidth = imageWidth * scale;
            float drawHeight = imageHeight * scale;

            float posX = ((float)gsGlobal->Width - drawWidth) / 2.0f;
            float posY = ((float)gsGlobal->Height - drawHeight) / 2.0f;

            gsKit_TexManager_bind(gsGlobal, currentMenu);

            gsKit_prim_sprite_texture(
                gsGlobal,
                currentMenu,
                posX, posY,
                0.0f, 0.0f,
                posX + drawWidth,
                posY + drawHeight,
                currentMenu->Width,
                currentMenu->Height,
                1,
                GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0)
            );
        }

        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }

    return 0;
}
