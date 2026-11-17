#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <libpad.h>
#include <stdio.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <audsrv.h>
#include <libmc.h>
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

/* Telas do jogo. */
#define SCREEN_MENU    0
#define SCREEN_OPTIONS 1
#define SCREEN_CREDITS 2
#define SCREEN_MULTI   3

/* Linhas da tela de opções. */
#define OPT_BRIGHT  0
#define OPT_LANG    1
#define OPT_MUSIC   2
#define OPT_MASTER  3
#define OPT_CREDITS 4
#define OPT_ROWS    5

/* Layout das imagens da tela de opções (640x448). */
#define OPT_ROW_X1   60.0f
#define OPT_ROW_X2   580.0f
#define OPT_ROW_HALF 23.0f
#define OPT_BAR_X    360.0f
#define OPT_SEG_W    17.0f
#define OPT_SEG_GAP  4.0f
#define OPT_SEG_H    18.0f

static const float optCenterY[OPT_ROWS] = {150.0f, 204.0f, 258.0f, 312.0f, 366.0f};

/* Tela de multiplayer: 2, 3 e 4 jogadores (imagem 640x448). */
#define MULTI_ROWS     3
#define MULTI_ROW_X1   120.0f
#define MULTI_ROW_X2   520.0f
#define MULTI_ROW_HALF 28.0f

static const float multiCenterY[MULTI_ROWS] = {170.0f, 245.0f, 320.0f};

/* Textura válida: carregou e cabe nos limites da GS. */
static int tex_ok(const GSTEXTURE *t)
{
    return t->Width > 0 && t->Height > 0 &&
           t->Width <= 1024 && t->Height <= 1024;
}

/* Idiomas: 0 = português, 1 = inglês, 2 = espanhol, 3 = japonês. */
#define LANGS 4

static const char *langCode[LANGS] = {"pt", "en", "es", "ja"};
static const char *btnName[MENU_COUNT] = {"jogar", "opcoes", "multiplayer", "sair"};

/* Todas as imagens que mudam com o idioma. */
typedef struct {
    GSTEXTURE menu;              int menuOk;    /* menu principal  */
    GSTEXTURE hl[MENU_COUNT];    int hlOk[MENU_COUNT];   /* destaques */
    GSTEXTURE opt;               int optOk;     /* tela de opções  */
    GSTEXTURE cred;              int credOk;    /* tela de créditos */
    GSTEXTURE multi;             int multiOk;   /* tela de multiplayer */
    GSTEXTURE dev;               int devOk;     /* aviso "em desenvolvimento" */
} LangAssets;

static LangAssets assets[LANGS];

static int load_png(GSGLOBAL *g, GSTEXTURE *t, const char *name)
{
    char path[64];

    snprintf(path, sizeof(path), "mass:/GRANNY/%s.png", name);
    memset(t, 0, sizeof(*t));
    t->Delayed = GS_SETTING_ON;

    if (gsKit_texture_png(g, t, path) == 0 && tex_ok(t))
        return 1;

    printf("ERRO: %s nao carregou\n", path);
    return 0;
}

static void load_assets(GSGLOBAL *g)
{
    char name[48];
    int l, b;

    for (l = 0; l < LANGS; l++)
    {
        snprintf(name, sizeof(name), "menu_%s", langCode[l]);
        assets[l].menuOk = load_png(g, &assets[l].menu, name);

        for (b = 0; b < MENU_COUNT; b++)
        {
            snprintf(name, sizeof(name), "destaque_%s_%s",
                     langCode[l], btnName[b]);
            assets[l].hlOk[b] = load_png(g, &assets[l].hl[b], name);
        }

        snprintf(name, sizeof(name), "opcoes_%s", langCode[l]);
        assets[l].optOk = load_png(g, &assets[l].opt, name);

        snprintf(name, sizeof(name), "creditos_%s", langCode[l]);
        assets[l].credOk = load_png(g, &assets[l].cred, name);

        snprintf(name, sizeof(name), "multi_%s", langCode[l]);
        assets[l].multiOk = load_png(g, &assets[l].multi, name);

        snprintf(name, sizeof(name), "dev_%s", langCode[l]);
        assets[l].devOk = load_png(g, &assets[l].dev, name);
    }
}

/* Efeito sonoro do cursor, misturado por software na música. */
#define SFX_VOICES 3
#define SFX_FADE   4410            /* amostras (~100 ms) de fade-out no fim */

/* Volumes ajustáveis na tela de opções (256 = 100%). */
static int musicGain = 256;
static int sfxGain   = 256;

/* musicVol = volume da música; masterVol = volume base (geral). 0..10 */
static void set_volumes(int musicVol, int masterVol)
{
    musicGain = musicVol * masterVol * 256 / 100;
    sfxGain   = masterVol * 256 / 10;
}

/* Brilho 0..10 (5 = original): escurece/clareia as imagens do jogo. */
static u64 tint(int level)
{
    int v = 0x80 * (40 + 12 * level) / 100;

    if (v > 0xFF)
        v = 0xFF;

    return GS_SETREG_RGBAQ(v, v, v, 0x80, 0);
}

static short sfxBuf[22050 * 2];   /* até 1 s estéreo a 22050 Hz */
static int sfxBytes = 0;
static int sfxPos[SFX_VOICES] = {-1, -1, -1};   /* -1 = voz livre */

/* Inicia o som sem cortar o anterior (evita estalo ao trocar rápido). */
static void sfx_trigger(void)
{
    int v, pick = 0;

    if (sfxBytes <= 0)
        return;

    for (v = 0; v < SFX_VOICES; v++)
    {
        if (sfxPos[v] < 0)
        {
            sfxPos[v] = 0;
            return;
        }
        if (sfxPos[v] > sfxPos[pick])
            pick = v;
    }

    sfxPos[pick] = 0;   /* reaproveita a voz mais antiga (já quase no fim) */
}

/* Aplica o ganho da música e soma as vozes do efeito, com limite. */
static void mix_audio(char *chunk, int bytes)
{
    short *dst = (short *)chunk;
    int n = bytes / 2;
    int total = sfxBytes / 2;
    int i, v;

    for (i = 0; i < n; i++)
    {
        int s = (dst[i] * musicGain) >> 8;

        for (v = 0; v < SFX_VOICES; v++)
        {
            if (sfxPos[v] >= 0)
            {
                int p = sfxPos[v];
                int smp = (sfxBuf[p] * sfxGain) >> 8;
                int rem = total - p;

                /* Fade-out garantido: o som nunca termina "cortado". */
                if (rem < SFX_FADE)
                    smp = smp * rem / SFX_FADE;

                s += smp;
                sfxPos[v] = p + 1;

                if (sfxPos[v] >= total)
                    sfxPos[v] = -1;
            }
        }

        if (s > 32767)  s = 32767;
        if (s < -32768) s = -32768;

        dst[i] = (short)s;
    }
}

/* Localiza o bloco "data" de um WAV (PCM 16 bits, estéreo, 22050 Hz).
   Posiciona o arquivo no início dos dados e devolve início e tamanho.
   Assim nenhum byte de cabeçalho/metadado é tocado como áudio. */
static int wav_open_data(FILE *f, long *dataStart, long *dataSize)
{
    unsigned char hdr[12], ch[8], fmt[16];
    int fmtOk = 0;

    if (fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, 12, f) != 12)
        return 0;

    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0)
        return 0;

    while (fread(ch, 1, 8, f) == 8)
    {
        unsigned long sz = (unsigned long)ch[4] |
                           ((unsigned long)ch[5] << 8) |
                           ((unsigned long)ch[6] << 16) |
                           ((unsigned long)ch[7] << 24);

        if (memcmp(ch, "fmt ", 4) == 0)
        {
            if (sz < 16 || fread(fmt, 1, 16, f) != 16)
                return 0;

            fmtOk = ((fmt[0] | (fmt[1] << 8)) == 1) &&                  /* PCM */
                    ((fmt[2] | (fmt[3] << 8)) == 2) &&                  /* estéreo */
                    ((fmt[4] | (fmt[5] << 8) | (fmt[6] << 16)) == 22050) &&
                    ((fmt[14] | (fmt[15] << 8)) == 16);                 /* 16 bits */

            if (fseek(f, (long)((sz - 16) + (sz & 1)), SEEK_CUR) != 0)
                return 0;
        }
        else if (memcmp(ch, "data", 4) == 0)
        {
            long cur, end;

            if (!fmtOk)
                return 0;

            cur = ftell(f);
            *dataStart = cur;
            *dataSize = (long)(sz & ~3UL);

            /* Se o cabeçalho prometer mais do que o arquivo tem, limita. */
            if (fseek(f, 0, SEEK_END) == 0)
            {
                end = ftell(f);
                if (end > cur && *dataSize > ((end - cur) & ~3L))
                    *dataSize = (end - cur) & ~3L;
            }
            if (fseek(f, cur, SEEK_SET) != 0)
                return 0;

            return *dataSize > 0;
        }
        else
        {
            if (fseek(f, (long)(sz + (sz & 1)), SEEK_CUR) != 0)
                return 0;
        }
    }

    return 0;
}

/* Lê o próximo bloco da música, voltando ao início exatamente no fim
   dos dados (laço sem emendas e sem tocar nada além do áudio). */
static int music_read(FILE *f, char *buf, int size, long dataStart,
                      long dataSize, long *remaining)
{
    int want = size;
    int got;

    if (*remaining <= 0)
    {
        fseek(f, dataStart, SEEK_SET);
        *remaining = dataSize;
    }

    if (want > *remaining)
        want = (int)*remaining;

    got = (int)fread(buf, 1, (size_t)want, f);

    if (got <= 0)
    {
        fseek(f, dataStart, SEEK_SET);
        *remaining = dataSize;
        return 0;
    }

    *remaining -= got;
    return got;
}

/* ---------- Configurações: memory card (principal) e pendrive (reserva) ---------- */
#define CONFIG_SIZE    10
#define CONFIG_VERSION 1

#define CONFIG_USB_PATH "mass:/GRANNY/config.dat"
#define USB_ICON_SYS    "mass:/GRANNY/icon.sys"
#define USB_ICON        "mass:/GRANNY/bastao2.ico"

#define MC_DIR      "/GRANNYCLASSIC"
#define MC_CONFIG   "/GRANNYCLASSIC/config.dat"
#define MC_ICON_SYS "/GRANNYCLASSIC/icon.sys"
#define MC_ICON     "/GRANNYCLASSIC/bastao2.ico"
#define MC_ICON_OLD1 "/GRANNYCLASSIC/granny.ico"  /* 1ª versão (quadrado) */
#define MC_ICON_OLD2 "/GRANNYCLASSIC/bastao.ico"  /* 2ª versão (taco pequeno) */

/* Modos de abertura do PS2 (iguais aos do fileio.h). */
#define MC_O_RDONLY 0x0001
#define MC_O_WRONLY 0x0002
#define MC_O_CREAT  0x0200
#define MC_O_TRUNC  0x0400

static int mcReady = 0;
static unsigned char mcBuf[2048] __attribute__((aligned(64)));

/* Formato (10 bytes): "GRNY", versão, brilho, idioma, música, base, soma. */
static unsigned char config_sum(const unsigned char *b)
{
    unsigned sum = 0;
    int i;

    for (i = 0; i < CONFIG_SIZE - 1; i++)
        sum += b[i];

    return (unsigned char)(sum & 0xFF);
}

static void config_pack(unsigned char *b, int brightness, int language,
                        int musicVol, int masterVol)
{
    memcpy(b, "GRNY", 4);
    b[4] = CONFIG_VERSION;
    b[5] = (unsigned char)brightness;
    b[6] = (unsigned char)language;
    b[7] = (unsigned char)musicVol;
    b[8] = (unsigned char)masterVol;
    b[9] = config_sum(b);
}

static int config_unpack(const unsigned char *b, int *brightness,
                         int *language, int *musicVol, int *masterVol)
{
    if (memcmp(b, "GRNY", 4) != 0 || b[4] != CONFIG_VERSION)
        return 0;
    if (b[CONFIG_SIZE - 1] != config_sum(b))
        return 0;
    if (b[5] > 10 || b[6] >= LANGS || b[7] > 10 || b[8] > 10)
        return 0;

    *brightness = b[5];
    *language   = b[6];
    *musicVol   = b[7];
    *masterVol  = b[8];
    return 1;
}

/* Inicia o memory card (slot 1). Devolve 1 se há um cartão de PS2 pronto. */
static int mc_start(void)
{
    int type = 0, freeBlocks = 0, format = 0, ret = -1;

    if (mcInit(MC_TYPE_XMC) < 0)
    {
        SifLoadModule("rom0:XSIO2MAN", 0, NULL);
        SifLoadModule("rom0:XMCMAN", 0, NULL);
        SifLoadModule("rom0:XMCSERV", 0, NULL);

        if (mcInit(MC_TYPE_XMC) < 0)
            return 0;
    }

    mcGetInfo(0, 0, &type, &freeBlocks, &format);
    mcSync(0, NULL, &ret);

    /* ret >= -1: cartão presente; tipo 2 = cartão de PlayStation 2 */
    return (ret >= -1 && type == 2);
}

static int mc_exists(const char *path)
{
    int fd = -1, res = 0;

    mcOpen(0, 0, path, MC_O_RDONLY);
    mcSync(0, NULL, &fd);

    if (fd < 0)
        return 0;

    mcClose(fd);
    mcSync(0, NULL, &res);
    return 1;
}

static int mc_write_file(const char *path, const void *data, int size)
{
    int fd = -1, res = -1, r2 = 0;

    if (size > (int)sizeof(mcBuf))
        return 0;

    memcpy(mcBuf, data, (size_t)size);

    mcOpen(0, 0, path, MC_O_WRONLY | MC_O_CREAT | MC_O_TRUNC);
    mcSync(0, NULL, &fd);

    if (fd < 0)
        return 0;

    mcWrite(fd, mcBuf, size);
    mcSync(0, NULL, &res);
    mcClose(fd);
    mcSync(0, NULL, &r2);

    return res == size;
}

/* Copia um arquivo do pendrive para o memory card (usado só no ícone). */
static int mc_copy_from_usb(const char *usbPath, const char *mcPath)
{
    FILE *f = fopen(usbPath, "rb");
    int fd = -1, res = -1, r2 = 0, ok = 1;
    size_t n;

    if (f == NULL)
        return 0;

    mcOpen(0, 0, mcPath, MC_O_WRONLY | MC_O_CREAT | MC_O_TRUNC);
    mcSync(0, NULL, &fd);

    if (fd < 0)
    {
        fclose(f);
        return 0;
    }

    while ((n = fread(mcBuf, 1, sizeof(mcBuf), f)) > 0)
    {
        mcWrite(fd, mcBuf, (int)n);
        mcSync(0, NULL, &res);

        if (res != (int)n)
        {
            ok = 0;
            break;
        }
    }

    mcClose(fd);
    mcSync(0, NULL, &r2);
    fclose(f);

    if (!ok)
    {
        mcDelete(0, 0, mcPath);      /* não deixa arquivo pela metade */
        mcSync(0, NULL, &r2);
    }

    return ok;
}

/* Instala o ícone (bastão com sangue) no save. O icon.sys vai por último,
   assim a pasta só aparece no navegador do PS2 com o ícone completo. */
static void mc_install_icon(void)
{
    int r = 0;

    if (mc_exists(MC_ICON) && mc_exists(MC_ICON_SYS))
        return;                              /* já instalado */

    /* Ícone novo primeiro; depois a ficha (icon.sys), sobrescrevendo a antiga. */
    if (!mc_exists(MC_ICON) && !mc_copy_from_usb(USB_ICON, MC_ICON))
        return;

    mc_copy_from_usb(USB_ICON_SYS, MC_ICON_SYS);

    /* Remove os ícones das versões anteriores, se existirem. */
    mcDelete(0, 0, MC_ICON_OLD1);
    mcSync(0, NULL, &r);
    mcDelete(0, 0, MC_ICON_OLD2);
    mcSync(0, NULL, &r);
}

static int config_load(int *brightness, int *language,
                       int *musicVol, int *masterVol)
{
    unsigned char b[CONFIG_SIZE];
    int fd = -1, res = -1, r2 = 0;
    size_t n;
    FILE *f;

    /* 1) memory card */
    if (mcReady)
    {
        mcOpen(0, 0, MC_CONFIG, MC_O_RDONLY);
        mcSync(0, NULL, &fd);

        if (fd >= 0)
        {
            mcRead(fd, mcBuf, CONFIG_SIZE);
            mcSync(0, NULL, &res);
            mcClose(fd);
            mcSync(0, NULL, &r2);

            if (res == CONFIG_SIZE)
            {
                memcpy(b, mcBuf, CONFIG_SIZE);

                if (config_unpack(b, brightness, language, musicVol, masterVol))
                    return 1;
            }
        }
    }

    /* 2) reserva: pendrive */
    f = fopen(CONFIG_USB_PATH, "rb");
    if (f != NULL)
    {
        n = fread(b, 1, CONFIG_SIZE, f);
        fclose(f);

        if (n == CONFIG_SIZE &&
            config_unpack(b, brightness, language, musicVol, masterVol))
            return 1;
    }

    return 0;
}

static int config_save(int brightness, int language,
                       int musicVol, int masterVol)
{
    unsigned char b[CONFIG_SIZE];
    int r = 0;
    FILE *f;
    size_t n;

    config_pack(b, brightness, language, musicVol, masterVol);

    /* 1) memory card (com o ícone do bastão) */
    if (mcReady)
    {
        mcMkDir(0, 0, MC_DIR);
        mcSync(0, NULL, &r);          /* se a pasta já existe, tudo bem */

        mc_install_icon();

        if (mc_write_file(MC_CONFIG, b, CONFIG_SIZE))
            return 1;
    }

    /* 2) reserva: pendrive */
    f = fopen(CONFIG_USB_PATH, "wb");
    if (f == NULL)
        return 0;

    n = fwrite(b, 1, CONFIG_SIZE, f);
    fclose(f);

    return n == CONFIG_SIZE;
}

int main(int argc, char *argv[])
{
    GSGLOBAL *gsGlobal;
    int screen = SCREEN_MENU;
    int optSel = 0;
    int multiSel = 0;       /* 0=2 jogadores, 1=3, 2=4 */
    int devPopup = 0;       /* 1 = mostrando "em desenvolvimento" */
    int brightness = 5;     /* 0..10, 5 = original */
    int language = 0;       /* 0=português 1=inglês 2=espanhol 3=japonês */
    int musicVol = 10;      /* 0..10 */
    int masterVol = 10;     /* 0..10 (volume base) */
    int cfgDirty = 0;       /* 1 = mudou algo e ainda não foi salvo */

    int selected = 0;
    int padOpened = 0;
    int state;
    int audioReady = 0;

    struct audsrv_fmt_t audioFormat;
    FILE *musicFile = NULL;
    char audioChunk[2048] __attribute__((aligned(16)));
    long musicDataStart = 0;
    long musicDataSize = 0;
    long musicRemaining = 0;
    int audioPendingSize = 0;

    struct padButtonStatus buttons;
    unsigned short oldButtons = 0;
    unsigned short currentButtons;
    unsigned short pressedButtons;

    int analogSet = 0;
    int oldStickUp = 0;
    int oldStickDown = 0;
    int oldStickLeft = 0;
    int oldStickRight = 0;

    u64 black = GS_SETREG_RGBAQ(5, 5, 8, 0, 0);
    u64 errorRed = GS_SETREG_RGBAQ(0xC0, 0x00, 0x00, 0x80, 0);

    /* RPC antes de qualquer acesso a arquivo ou módulo. */
    sceSifInitRpc(0);

    /* Memory card (save principal) e configurações salvas. */
    mcReady = mc_start();

    if (config_load(&brightness, &language, &musicVol, &masterVol))
        set_volumes(musicVol, masterVol);

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

    /* Carrega as imagens de todos os idiomas antes de iniciar o áudio,
       para trocar de idioma depois sem pausas. */
    load_assets(gsGlobal);

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

                if (musicFile != NULL &&
                    wav_open_data(musicFile, &musicDataStart, &musicDataSize))
                {
                    musicRemaining = musicDataSize;
                }
                else
                {
                    if (musicFile != NULL)
                    {
                        fclose(musicFile);
                        musicFile = NULL;
                    }
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
            long sStart = 0, sSize = 0;

            if (wav_open_data(sfx, &sStart, &sSize))
            {
                if (sSize > (long)sizeof(sfxBuf))
                    sSize = (long)sizeof(sfxBuf);

                sfxBytes = (int)fread(sfxBuf, 1, (size_t)sSize, sfx);
                sfxBytes &= ~3;   /* múltiplo de um quadro estéreo */
            }

            fclose(sfx);
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
        for (int pump = 0; pump < 3 && audioReady && musicFile != NULL; pump++)
        {
            if (audioPendingSize == 0 &&
                audsrv_available() >= (int)sizeof(audioChunk))
            {
                audioPendingSize = music_read(
                    musicFile, audioChunk, (int)sizeof(audioChunk),
                    musicDataStart, musicDataSize, &musicRemaining);

                if (audioPendingSize > 0)
                    mix_audio(audioChunk, audioPendingSize);
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

            if (audioPendingSize > 0)
                break;   /* buffer cheio: tenta de novo no próximo quadro */
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
                    int stickLeft = 0, stickRight = 0;

                    /* modo 7 = analógico ativo (só o analógico esquerdo) */
                    if ((buttons.mode >> 4) == 0x07)
                    {
                        stickUp    = buttons.ljoy_v < STICK_LOW;
                        stickDown  = buttons.ljoy_v > STICK_HIGH;
                        stickLeft  = buttons.ljoy_h < STICK_LOW;
                        stickRight = buttons.ljoy_h > STICK_HIGH;
                    }

                    /* Um movimento por empurrão, igual aos direcionais. */
                    int goUp    = (pressedButtons & PAD_UP) ||
                                  (stickUp && !oldStickUp);
                    int goDown  = (pressedButtons & PAD_DOWN) ||
                                  (stickDown && !oldStickDown);
                    int goLeft  = (pressedButtons & PAD_LEFT) ||
                                  (stickLeft && !oldStickLeft);
                    int goRight = (pressedButtons & PAD_RIGHT) ||
                                  (stickRight && !oldStickRight);

                    oldStickUp = stickUp;
                    oldStickDown = stickDown;
                    oldStickLeft = stickLeft;
                    oldStickRight = stickRight;

                    if (screen == SCREEN_MENU)
                    {
                        if (goUp)
                        {
                            selected = (selected + MENU_COUNT - 1) % MENU_COUNT;
                            sfx_trigger();
                        }
                        else if (goDown)
                        {
                            selected = (selected + 1) % MENU_COUNT;
                            sfx_trigger();
                        }

                        if (pressedButtons & PAD_CROSS)
                        {
                            if (selected == 1)          /* OPÇÕES */
                            {
                                screen = SCREEN_OPTIONS;
                                optSel = 0;
                                sfx_trigger();
                            }
                            else if (selected == 2)     /* MULTIPLAYER */
                            {
                                screen = SCREEN_MULTI;
                                multiSel = 0;
                                devPopup = 0;
                                sfx_trigger();
                            }
                            else if (selected == 3)     /* SAIR */
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
                    else if (screen == SCREEN_OPTIONS)
                    {
                        if (goUp)
                        {
                            optSel = (optSel + OPT_ROWS - 1) % OPT_ROWS;
                            sfx_trigger();
                        }
                        else if (goDown)
                        {
                            optSel = (optSel + 1) % OPT_ROWS;
                            sfx_trigger();
                        }

                        if (goLeft || goRight)
                        {
                            int d = goRight ? 1 : -1;

                            if (optSel != OPT_CREDITS)
                                cfgDirty = 1;

                            if (optSel == OPT_BRIGHT)
                            {
                                brightness += d;
                                if (brightness < 0)  brightness = 0;
                                if (brightness > 10) brightness = 10;
                            }
                            else if (optSel == OPT_LANG)
                            {
                                language = (language + d + LANGS) % LANGS;
                                sfx_trigger();
                            }
                            else if (optSel == OPT_MUSIC)
                            {
                                musicVol += d;
                                if (musicVol < 0)  musicVol = 0;
                                if (musicVol > 10) musicVol = 10;
                                set_volumes(musicVol, masterVol);
                            }
                            else if (optSel == OPT_MASTER)
                            {
                                masterVol += d;
                                if (masterVol < 0)  masterVol = 0;
                                if (masterVol > 10) masterVol = 10;
                                set_volumes(musicVol, masterVol);
                                sfx_trigger();   /* prévia do volume */
                            }
                        }

                        if ((pressedButtons & PAD_CROSS) && optSel == OPT_CREDITS)
                        {
                            screen = SCREEN_CREDITS;
                            sfx_trigger();
                        }
                        else if ((pressedButtons & PAD_CROSS) && optSel == OPT_LANG)
                        {
                            language = (language + 1) % LANGS;
                            cfgDirty = 1;
                            sfx_trigger();
                        }

                        if (pressedButtons & PAD_CIRCLE)
                        {
                            if (cfgDirty)
                            {
                                config_save(brightness, language,
                                            musicVol, masterVol);
                                cfgDirty = 0;
                            }

                            screen = SCREEN_MENU;
                            sfx_trigger();
                        }
                    }
                    else if (screen == SCREEN_MULTI)
                    {
                        if (devPopup)
                        {
                            /* Aviso aberto: X ou Círculo fecham. */
                            if (pressedButtons & (PAD_CIRCLE | PAD_CROSS))
                            {
                                devPopup = 0;
                                sfx_trigger();
                            }
                        }
                        else
                        {
                            if (goUp)
                            {
                                multiSel = (multiSel + MULTI_ROWS - 1) % MULTI_ROWS;
                                sfx_trigger();
                            }
                            else if (goDown)
                            {
                                multiSel = (multiSel + 1) % MULTI_ROWS;
                                sfx_trigger();
                            }

                            if (pressedButtons & PAD_CROSS)
                            {
                                /* 2, 3 ou 4 jogadores: ainda em desenvolvimento. */
                                devPopup = 1;
                                sfx_trigger();
                            }
                            else if (pressedButtons & PAD_CIRCLE)
                            {
                                screen = SCREEN_MENU;
                                sfx_trigger();
                            }
                        }
                    }
                    else /* SCREEN_CREDITS */
                    {
                        if (pressedButtons & (PAD_CIRCLE | PAD_CROSS))
                        {
                            screen = SCREEN_OPTIONS;
                            sfx_trigger();
                        }
                    }
                }
            }
        }

        /* ---------- Vídeo ---------- */
        gsKit_clear(gsGlobal, black);

        if (screen == SCREEN_CREDITS)
        {
            if (assets[language].credOk)
            {
                gsKit_TexManager_bind(gsGlobal, &assets[language].cred);
                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &assets[language].cred,
                    0.0f, 0.0f,
                    0.0f, 0.0f,
                    (float)gsGlobal->Width,
                    (float)gsGlobal->Height,
                    (float)assets[language].cred.Width,
                    (float)assets[language].cred.Height,
                    1,
                    tint(brightness)
                );
            }
        }
        else if (screen == SCREEN_MULTI)
        {
            float sx = (float)gsGlobal->Width  / 640.0f;
            float sy = (float)gsGlobal->Height / 448.0f;
            u64 frame = GS_SETREG_RGBAQ(0xD0, 0x20, 0x20, 0x80, 0);
            float fy1 = (multiCenterY[multiSel] - MULTI_ROW_HALF) * sy;
            float fy2 = (multiCenterY[multiSel] + MULTI_ROW_HALF) * sy;
            float fx1 = MULTI_ROW_X1 * sx;
            float fx2 = MULTI_ROW_X2 * sx;
            float t = 2.0f * sy;

            if (assets[language].multiOk)
            {
                gsKit_TexManager_bind(gsGlobal, &assets[language].multi);
                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &assets[language].multi,
                    0.0f, 0.0f,
                    0.0f, 0.0f,
                    (float)gsGlobal->Width,
                    (float)gsGlobal->Height,
                    (float)assets[language].multi.Width,
                    (float)assets[language].multi.Height,
                    1,
                    tint(brightness)
                );
            }

            /* Moldura vermelha na opção selecionada. */
            gsKit_prim_sprite(gsGlobal, fx1, fy1, fx2, fy1 + t, 3, frame);
            gsKit_prim_sprite(gsGlobal, fx1, fy2 - t, fx2, fy2, 3, frame);
            gsKit_prim_sprite(gsGlobal, fx1, fy1, fx1 + t, fy2, 3, frame);
            gsKit_prim_sprite(gsGlobal, fx2 - t, fy1, fx2, fy2, 3, frame);

            /* Aviso "em desenvolvimento" no centro da tela. */
            if (devPopup && assets[language].devOk)
            {
                GSTEXTURE *dv = &assets[language].dev;
                float pw = (float)dv->Width;
                float ph = (float)dv->Height;
                float px1 = ((640.0f - pw) / 2.0f) * sx;
                float py1 = ((448.0f - ph) / 2.0f) * sy;

                gsKit_TexManager_bind(gsGlobal, dv);
                gsKit_prim_sprite_texture(
                    gsGlobal,
                    dv,
                    px1, py1,
                    0.0f, 0.0f,
                    px1 + pw * sx, py1 + ph * sy,
                    pw, ph,
                    4,
                    tint(brightness)
                );
            }
        }
        else if (screen == SCREEN_OPTIONS)
        {
            float sx = (float)gsGlobal->Width  / 640.0f;
            float sy = (float)gsGlobal->Height / 448.0f;
            u64 barOn  = GS_SETREG_RGBAQ(0xB4, 0x12, 0x12, 0x80, 0);
            u64 barOff = GS_SETREG_RGBAQ(0x2A, 0x28, 0x28, 0x80, 0);
            u64 frame  = GS_SETREG_RGBAQ(0xD0, 0x20, 0x20, 0x80, 0);
            const int barRow[3] = {OPT_BRIGHT, OPT_MUSIC, OPT_MASTER};
            const int barVal[3] = {brightness, musicVol, masterVol};
            float fy1 = (optCenterY[optSel] - OPT_ROW_HALF) * sy;
            float fy2 = (optCenterY[optSel] + OPT_ROW_HALF) * sy;
            float fx1 = OPT_ROW_X1 * sx;
            float fx2 = OPT_ROW_X2 * sx;
            float t = 2.0f * sy;
            int r, i;

            if (assets[language].optOk)
            {
                gsKit_TexManager_bind(gsGlobal, &assets[language].opt);
                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &assets[language].opt,
                    0.0f, 0.0f,
                    0.0f, 0.0f,
                    (float)gsGlobal->Width,
                    (float)gsGlobal->Height,
                    (float)assets[language].opt.Width,
                    (float)assets[language].opt.Height,
                    1,
                    tint(brightness)
                );
            }

            /* Barras de 10 segmentos: brilho, volume da música, volume base. */
            for (r = 0; r < 3; r++)
            {
                float by = (optCenterY[barRow[r]] - OPT_SEG_H / 2.0f) * sy;

                for (i = 0; i < 10; i++)
                {
                    float bx = (OPT_BAR_X + i * (OPT_SEG_W + OPT_SEG_GAP)) * sx;

                    gsKit_prim_sprite(
                        gsGlobal,
                        bx, by,
                        bx + OPT_SEG_W * sx, by + OPT_SEG_H * sy,
                        2, (i < barVal[r]) ? barOn : barOff);
                }
            }

            /* Moldura vermelha na linha selecionada. */
            gsKit_prim_sprite(gsGlobal, fx1, fy1, fx2, fy1 + t, 3, frame);
            gsKit_prim_sprite(gsGlobal, fx1, fy2 - t, fx2, fy2, 3, frame);
            gsKit_prim_sprite(gsGlobal, fx1, fy1, fx1 + t, fy2, 3, frame);
            gsKit_prim_sprite(gsGlobal, fx2 - t, fy1, fx2, fy2, 3, frame);
        }
        else if (assets[language].menuOk)
        {
            /* Fatores de escala: as posições foram medidas em 640x448,
               mas o modo de vídeo pode ser PAL (640x512). */
            float sx = (float)gsGlobal->Width  / 640.0f;
            float sy = (float)gsGlobal->Height / 448.0f;

            gsKit_TexManager_bind(gsGlobal, &assets[language].menu);
            gsKit_prim_sprite_texture(
                gsGlobal,
                &assets[language].menu,
                0.0f, 0.0f,
                0.0f, 0.0f,
                (float)gsGlobal->Width,
                (float)gsGlobal->Height,
                (float)assets[language].menu.Width,
                (float)assets[language].menu.Height,
                1,
                tint(brightness)
            );

            /* Destaque opaco só sobre o botão selecionado. */
            if (assets[language].hlOk[selected])
            {
                float x1 = buttonPos[selected].x * sx;
                float y1 = buttonPos[selected].y * sy;
                float x2 = x1 + (float)assets[language].hl[selected].Width  * sx;
                float y2 = y1 + (float)assets[language].hl[selected].Height * sy;

                gsKit_TexManager_bind(gsGlobal, &assets[language].hl[selected]);
                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &assets[language].hl[selected],
                    x1, y1,
                    0.0f, 0.0f,
                    x2, y2,
                    (float)assets[language].hl[selected].Width,
                    (float)assets[language].hl[selected].Height,
                    2,
                    tint(brightness)
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
