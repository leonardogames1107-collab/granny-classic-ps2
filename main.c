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
#include <stdlib.h>
#include <time.h>

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
#define SCREEN_PLAY    4
#define SCREEN_GAME    5

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

/* ---------- Tela de JOGAR (configuração da partida) ---------- */
#define DIFFS 5

/* Opções definidas pela dificuldade (travadas):
   escuro, cadeados extras, mancando, música. */
static const unsigned char diffPreset[DIFFS][4] = {
    {0, 0, 1, 1},   /* Fácil   */
    {0, 0, 0, 1},   /* Normal  */
    {1, 1, 0, 1},   /* Difícil */
    {1, 1, 0, 0},   /* Extremo */
    {0, 0, 0, 1}    /* Prática (a Granny não aparece) */
};

enum { PI_DIFF, PI_GSKIN, PI_PSKIN, PI_ACH, PI_BACK, PI_SAVE, PI_CONT, PI_COUNT };

/* Retângulos dos itens (x1, y1, x2, y2) na imagem 640x448. */
static const float playRect[PI_COUNT][4] = {
    { 24.0f, 186.0f, 156.0f, 218.0f},   /* dificuldade           */
    {190.0f,  56.0f, 254.0f, 120.0f},   /* skins da Granny       */
    {270.0f,  56.0f, 334.0f, 120.0f},   /* skins do jogador      */
    {540.0f,  40.0f, 612.0f, 112.0f},   /* conquistas            */
    { 24.0f, 318.0f, 134.0f, 352.0f},   /* voltar                */
    { 24.0f, 366.0f, 134.0f, 400.0f},   /* salvar                */
    {500.0f, 318.0f, 616.0f, 352.0f}    /* continuar             */
};

/* Navegação: cima, baixo, esquerda, direita.
   -1 = fica onde está; -2 = ajusta a dificuldade. */
static const signed char playNav[PI_COUNT][4] = {
    /* DIFF  */ {PI_GSKIN, PI_BACK, -2, -2},
    /* GSKIN */ {-1, PI_DIFF, PI_DIFF, PI_PSKIN},
    /* PSKIN */ {-1, PI_DIFF, PI_GSKIN, PI_ACH},
    /* ACH   */ {-1, PI_CONT, PI_PSKIN, -1},
    /* BACK  */ {PI_DIFF, PI_SAVE, -1, PI_CONT},
    /* SAVE  */ {PI_BACK, -1, -1, PI_CONT},
    /* CONT  */ {PI_ACH, -1, PI_BACK, -1}
};

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
    GSTEXTURE play;              int playOk;    /* tela de JOGAR (fundo) */
    GSTEXTURE atlas;             int atlasOk;   /* dificuldades, avisos de save */
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

/* Carrega todas as imagens de um idioma. */
static void load_lang(GSGLOBAL *g, int l)
{
    char name[48];
    int b;

    snprintf(name, sizeof(name), "menu_%s", langCode[l]);
    assets[l].menuOk = load_png(g, &assets[l].menu, name);

    for (b = 0; b < MENU_COUNT; b++)
    {
        snprintf(name, sizeof(name), "destaque_%s_%s", langCode[l], btnName[b]);
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

    snprintf(name, sizeof(name), "jogar_%s", langCode[l]);
    assets[l].playOk = load_png(g, &assets[l].play, name);

    snprintf(name, sizeof(name), "jogarx_%s", langCode[l]);
    assets[l].atlasOk = load_png(g, &assets[l].atlas, name);
}

static void load_assets(GSGLOBAL *g)
{
    int l;

    for (l = 0; l < LANGS; l++)
        load_lang(g, l);
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
static int fadeLevel = 256;      /* 0..256: entrada suave do menu depois da abertura */

static u64 tint(int level)
{
    int v = 0x80 * (40 + 12 * level) / 100;

    if (v > 0xFF)
        v = 0xFF;

    v = v * fadeLevel / 256;

    return GS_SETREG_RGBAQ(v, v, v, 0x80, 0);
}

static short cfmBuf[22050 * 2];   /* som de confirmação (X) */
static int cfmBytes = 0;
static int cfmPos[SFX_VOICES] = {-1, -1, -1};

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

/* Som de confirmação (X). Se o arquivo não existir, usa o som do cursor. */
static void sfx_confirm(void)
{
    int v, pick = 0;

    if (cfmBytes <= 0)
    {
        sfx_trigger();
        return;
    }

    for (v = 0; v < SFX_VOICES; v++)
    {
        if (cfmPos[v] < 0)
        {
            cfmPos[v] = 0;
            return;
        }
        if (cfmPos[v] > cfmPos[pick])
            pick = v;
    }

    cfmPos[pick] = 0;
}

/* Aplica o ganho da música e soma as vozes do efeito, com limite. */
static void mix_audio(char *chunk, int bytes)
{
    short *dst = (short *)chunk;
    int n = bytes / 2;
    int total = sfxBytes / 2;
    int ctotal = cfmBytes / 2;
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

        for (v = 0; v < SFX_VOICES; v++)
        {
            if (cfmPos[v] >= 0)
            {
                int p = cfmPos[v];
                int smp = (cfmBuf[p] * sfxGain) >> 8;
                int rem = ctotal - p;

                if (rem < SFX_FADE)
                    smp = smp * rem / SFX_FADE;

                s += smp;
                cfmPos[v] = p + 1;

                if (cfmPos[v] >= ctotal)
                    cfmPos[v] = -1;
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
#define CONFIG_SIZE    26
#define CONFIG_VERSION 2

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

/* Dados salvos: preferências + dados da partida (dificuldade, skins, conquistas). */
typedef struct {
    int brightness, language, musicVol, masterVol;
    int difficulty, grannySkin, playerSkin;
    unsigned int grannyUnlocked, playerUnlocked, achievements;
} SaveData;

static void save_defaults(SaveData *d)
{
    d->brightness = 5;  d->language = 0;
    d->musicVol = 10;   d->masterVol = 10;
    d->difficulty = 1;  d->grannySkin = 0;  d->playerSkin = 0;
    d->grannyUnlocked = 1;  d->playerUnlocked = 1;  d->achievements = 0;
}

/* Formato v2 (26 bytes): "GRNY", versão, brilho, idioma, música, base,
   dificuldade, skin da Granny, skin do jogador, skins liberadas da Granny
   (4 bytes), skins liberadas do jogador (4), conquistas (4), reservado, soma.
   O formato v1 (10 bytes, só as preferências) ainda é lido. */
static unsigned char config_sum(const unsigned char *b, int n)
{
    unsigned sum = 0;
    int i;

    for (i = 0; i < n - 1; i++)
        sum += b[i];

    return (unsigned char)(sum & 0xFF);
}

static void put32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static unsigned int get32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void config_pack(unsigned char *b, const SaveData *d)
{
    memset(b, 0, CONFIG_SIZE);
    memcpy(b, "GRNY", 4);
    b[4]  = CONFIG_VERSION;
    b[5]  = (unsigned char)d->brightness;
    b[6]  = (unsigned char)d->language;
    b[7]  = (unsigned char)d->musicVol;
    b[8]  = (unsigned char)d->masterVol;
    b[9]  = (unsigned char)d->difficulty;
    b[10] = (unsigned char)d->grannySkin;
    b[11] = (unsigned char)d->playerSkin;
    put32(b + 12, d->grannyUnlocked);
    put32(b + 16, d->playerUnlocked);
    put32(b + 20, d->achievements);
    b[CONFIG_SIZE - 1] = config_sum(b, CONFIG_SIZE);
}

/* d já deve estar com os valores padrão; só é alterado se o save for válido. */
static int config_unpack(const unsigned char *b, int n, SaveData *d)
{
    if (n < 10 || memcmp(b, "GRNY", 4) != 0)
        return 0;
    if (b[n - 1] != config_sum(b, n))
        return 0;
    if (b[5] > 10 || b[6] >= LANGS || b[7] > 10 || b[8] > 10)
        return 0;

    if (b[4] == 1 && n == 10)                 /* save antigo */
    {
        d->brightness = b[5];
        d->language   = b[6];
        d->musicVol   = b[7];
        d->masterVol  = b[8];
        return 1;
    }

    if (b[4] == CONFIG_VERSION && n == CONFIG_SIZE)
    {
        if (b[9] >= DIFFS || b[10] >= 32 || b[11] >= 32)
            return 0;

        d->brightness = b[5];
        d->language   = b[6];
        d->musicVol   = b[7];
        d->masterVol  = b[8];
        d->difficulty = b[9];
        d->grannySkin = b[10];
        d->playerSkin = b[11];
        d->grannyUnlocked = get32(b + 12);
        d->playerUnlocked = get32(b + 16);
        d->achievements   = get32(b + 20);
        return 1;
    }

    return 0;
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

static int config_load(SaveData *d)
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

            if (res >= 10 && res <= CONFIG_SIZE)
            {
                memcpy(b, mcBuf, (size_t)res);

                if (config_unpack(b, res, d))
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

        if (n >= 10 && config_unpack(b, (int)n, d))
            return 1;
    }

    return 0;
}

static int config_save(const SaveData *d)
{
    unsigned char b[CONFIG_SIZE];
    int r = 0;
    FILE *f;
    size_t n;

    config_pack(b, d);

    /* 1) memory card (com o ícone do taco) */
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

/* Desenha um pedaço (u, v, w, h em texels) de uma imagem na posição x, y
   (coordenadas da tela 640x448). */
static void draw_sub(GSGLOBAL *g, GSTEXTURE *t, float x, float y,
                     float w, float h, float u, float v, u64 color, int z)
{
    float sx = (float)g->Width  / 640.0f;
    float sy = (float)g->Height / 448.0f;

    gsKit_TexManager_bind(g, t);
    gsKit_prim_sprite_texture(
        g, t,
        x * sx, y * sy, u, v,
        (x + w) * sx, (y + h) * sy, u + w, v + h,
        z, color);
}

/* ---------- Abertura: vídeo (quadros PNG) + áudio ---------- */
#define INTRO_FPS     20          /* quadros por segundo dos arquivos intro/iNNNN.png */
#define INTRO_BG_LOAD 0           /* 0 = carrega o menu ANTES da abertura (seguro); 1 = em segundo plano */
#define INTRO_FRAMES  343         /* total de quadros intro/iNNNN.png */
#define INTRO_WAV     "mass:/GRANNY/intro.wav"

static volatile int loaderDone = 0;      /* 1 = todas as imagens carregadas */
static GSTEXTURE introTex[2];            /* dois quadros alternados (fixos na memória) */

static void clear_screen(GSGLOBAL *g, u64 c)
{
    gsKit_clear(g, c);
    gsKit_set_test(g, GS_ZTEST_OFF);
}

static unsigned int cpu_ticks_now(void)
{
#if defined(__mips__)
    unsigned int c;

    __asm__ __volatile__("mfc0 %0, $9" : "=r"(c));   /* contador da CPU */
    return c;
#else
    return (unsigned int)clock();
#endif
}

/* Mede quantos "ticks" do contador passam por segundo, usando o vsync. */
static double measure_tick_rate(GSGLOBAL *g, u64 black, double refresh)
{
    unsigned int t0, t1;
    double tps;
    int i;

    for (i = 0; i < 3; i++)
    {
        clear_screen(g, black);
        gsKit_sync_flip(g);
        gsKit_queue_exec(g);
    }

    t0 = cpu_ticks_now();

    for (i = 0; i < 20; i++)
    {
        clear_screen(g, black);
        gsKit_sync_flip(g);
        gsKit_queue_exec(g);
    }

    t1 = cpu_ticks_now();
    tps = (double)(unsigned int)(t1 - t0) / (20.0 / refresh);

    if (tps < 5.0e7 || tps > 6.0e8)
        tps = 294912000.0;                 /* valor típico do EE */

    return tps;
}

static int introFlat = 0;      /* 1 = quadros soltos na pasta GRANNY (sem subpasta) */

/* Procura os quadros em GRANNY/intro/ e, se o 1º não estiver lá,
   em GRANNY/ (soltos). Quem achar primeiro vale para a abertura toda. */
static int intro_load_frame(GSGLOBAL *g, GSTEXTURE *t, int n)
{
    char path[64];
    int pass;

    for (pass = 0; pass < 2; pass++)
    {
        snprintf(path, sizeof(path),
                 introFlat ? "mass:/GRANNY/i%04d.png"
                           : "mass:/GRANNY/intro/i%04d.png", n);

        memset(t, 0, sizeof(*t));
        t->Delayed = GS_SETTING_ON;

        if (gsKit_texture_png(g, t, path) == 0)
        {
            t->Filter = GS_FILTER_LINEAR;          /* suaviza o vídeo ampliado */
            gsKit_TexManager_invalidate(g, t);     /* força o envio do novo quadro à VRAM */
            return 1;
        }

        if (n != 1 || introFlat)
            return 0;                              /* acabaram os quadros */

        introFlat = 1;                             /* 1º quadro: tenta soltos na pasta */
    }

    return 0;
}

/* Toca a abertura. X ou START pulam. Devolve os botões segurados no fim,
   para o menu não "herdar" o X que pulou a abertura. */
static unsigned short play_intro(GSGLOBAL *g, int audioReady, int padOpened)
{
    static char chunk[2048] __attribute__((aligned(16)));
    struct padButtonStatus pb;
    FILE *wav = NULL;
    long wStart = 0, wSize = 0, wRemain = 0;
    int pending = 0, ok[2] = {0, 0}, cur = 0, shown = 0;
    unsigned short held = 0;
    double tps, elapsed = 0.0;
    unsigned int last, now;
    u64 black = GS_SETREG_RGBAQ(0, 0, 0, 0, 0);
    u64 white = GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0);
    double refresh = (g->Height >= 512) ? 50.0 : 59.94;

    tps = measure_tick_rate(g, black, refresh);

    if (audioReady)
    {
        wav = fopen(INTRO_WAV, "rb");

        if (wav != NULL && wav_open_data(wav, &wStart, &wSize))
        {
            wRemain = wSize;
        }
        else
        {
            if (wav != NULL)
                fclose(wav);
            wav = NULL;
        }
    }

    last = cpu_ticks_now();

    for (;;)
    {
        int frame, pump;

        now = cpu_ticks_now();
        elapsed += (double)(unsigned int)(now - last) / tps;
        last = now;

        /* áudio da abertura (toca uma vez, sem repetir) */
        for (pump = 0; pump < 3 && wav != NULL; pump++)
        {
            if (pending == 0 && wRemain > 0 &&
                audsrv_available() >= (int)sizeof(chunk))
            {
                int want = (wRemain < (long)sizeof(chunk)) ? (int)wRemain
                                                            : (int)sizeof(chunk);
                int got = (int)fread(chunk, 1, (size_t)want, wav);

                if (got <= 0)
                {
                    wRemain = 0;
                }
                else
                {
                    short *smp = (short *)chunk;
                    int i;

                    for (i = 0; i < got / 2; i++)
                        smp[i] = (short)((smp[i] * musicGain) >> 8);

                    wRemain -= got;
                    pending = got;
                }
            }

            if (pending > 0 && audsrv_available() >= pending)
            {
                int sent = audsrv_play_audio(chunk, pending);

                if (sent > 0)
                {
                    if (sent < pending)
                        memmove(chunk, chunk + sent, (size_t)(pending - sent));

                    pending -= sent;
                }
            }

            if (pending > 0)
                break;
        }

        /* vídeo: o quadro certo para o tempo decorrido (pula se atrasar) */
        frame = (int)(elapsed * INTRO_FPS) + 1;

        if (frame != shown)
        {
            int idx = 1 - cur;

            if (ok[idx])
            {
                free(introTex[idx].Mem);          /* quadro de 2 voltas atrás */
                free(introTex[idx].Clut);
                ok[idx] = 0;
            }

            if (frame > INTRO_FRAMES)
                break;                            /* fim do vídeo */

            if (intro_load_frame(g, &introTex[idx], frame))
            {
                ok[idx] = 1;
                cur = idx;                        /* quadro novo na tela */
            }
            /* se um quadro falhar, continua mostrando o anterior */

            shown = frame;

            if (!ok[0] && !ok[1] && frame > 10)
                break;                            /* não achou nenhum quadro: desiste */
        }

        clear_screen(g, black);

        if (ok[cur])
        {
            gsKit_TexManager_bind(g, &introTex[cur]);
            gsKit_prim_sprite_texture(
                g, &introTex[cur],
                0.0f, 0.0f,
                0.0f, 0.0f,
                (float)g->Width, (float)g->Height,
                (float)introTex[cur].Width, (float)introTex[cur].Height,
                1, white);
        }

        gsKit_sync_flip(g);
        gsKit_queue_exec(g);

        /* X ou START pulam a abertura */
        if (padOpened && padGetState(0, 0) == PAD_STATE_STABLE &&
            padRead(0, 0, &pb) != 0)
        {
            held = (unsigned short)(~pb.btns);

            if (held & (PAD_CROSS | PAD_START))
                break;
        }
    }

    if (ok[0]) { free(introTex[0].Mem); free(introTex[0].Clut); }
    if (ok[1]) { free(introTex[1].Mem); free(introTex[1].Clut); }
    memset(introTex, 0, sizeof(introTex));

    if (wav != NULL)
        fclose(wav);

    return held;
}

#if INTRO_BG_LOAD
extern void *_gp;

static GSGLOBAL *loaderG = NULL;
static int loaderFirst = 0;
static char loaderStack[64 * 1024] __attribute__((aligned(16)));

/* Carrega as imagens em segundo plano (prioridade baixa: só usa o tempo
   em que o jogo está esperando o vsync). */
static void loader_thread(void *arg)
{
    int i;

    (void)arg;

    for (i = 0; i < LANGS; i++)
        load_lang(loaderG, (loaderFirst + i) % LANGS);

    loaderDone = 1;
    ExitThread();
}

static int start_loader(GSGLOBAL *g, int firstLang)
{
    ee_thread_t th;
    int id;

    loaderG = g;
    loaderFirst = firstLang;
    loaderDone = 0;

    memset(&th, 0, sizeof(th));
    th.func = (void *)loader_thread;
    th.stack = loaderStack;
    th.stack_size = sizeof(loaderStack);
    th.gp_reg = &_gp;
    th.initial_priority = 64;

    id = CreateThread(&th);
    if (id < 0)
        return 0;

    if (StartThread(id, NULL) < 0)
    {
        DeleteThread(id);
        return 0;
    }

    return 1;
}
#endif

/* =====================================================================
   CENA 3D (etapa 1): câmera em primeira pessoa, colisão e agachar.
   Tudo é calculado na CPU e desenhado com triângulos coloridos (sem
   Z-buffer, ordenados de trás para frente), para não gastar VRAM.
   Eixos: x = leste, y = cima, z = norte. Unidade = metro.
   ===================================================================== */
#define G_PI          3.14159265f
#define G_NEAR        0.10f
#define G_MAXQ        2000
#define G_FLUSH_TRIS  100      /* esvazia a fila de desenho a cada N triângulos (0 = nunca) */
#define G_STAND_H     1.70f    /* altura do corpo em pé */
#define G_CROUCH_H    0.95f    /* altura do corpo agachado */
#define G_EYE_STAND   1.60f
#define G_EYE_CROUCH  0.85f
#define G_RADIUS      0.28f
#define G_CEIL        2.60f

typedef struct { float c[4][3]; float n[3]; unsigned char r, g, b, layer; } GQuad;
typedef struct { float x0, y0, z0, x1, y1, z1; } GBox;
typedef struct { float x, y, z, b; } GV;
typedef struct { float key; int idx; } GSortItem;

static GQuad gQuads[G_MAXQ];
static int   gQuadN = 0;
static GBox  gBoxes[96];
static int   gBoxN = 0;
static unsigned int gSeed = 12345u;

/* Estado do jogador. */
static float gPX, gPZ, gYaw, gPitch, gEye;
static int   gCrouch = 0;
static int   gCrouchReq = 0;
static int   gExitReq = 0;
static int   gSceneBuilt = 0;
static double gTps = 0.0;
static unsigned int gLastTick = 0;

/* Controle (preenchido pelo laço principal). */
static int gPadLX = 128, gPadLY = 128, gPadRX = 128, gPadRY = 128;
static unsigned short gPadHeld = 0;

/* ---- matemática própria (não depende da libm) ---- */
static float g_sqrt(float x)
{
    union { float f; unsigned int u; } c;
    float y;

    if (x <= 0.0f)
        return 0.0f;

    c.f = x;
    c.u = 0x1fbd1df5u + (c.u >> 1);
    y = c.f;
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    return y;
}

static float g_sin(float x)
{
    float x2;

    while (x > G_PI)  x -= 2.0f * G_PI;
    while (x < -G_PI) x += 2.0f * G_PI;

    if (x > G_PI * 0.5f)       x = G_PI - x;
    else if (x < -G_PI * 0.5f) x = -G_PI - x;

    x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f +
           x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}

static float g_cos(float x) { return g_sin(x + G_PI * 0.5f); }

static float g_axis(int v)
{
    float a = ((float)v - 128.0f) / 128.0f;

    if (a > -0.18f && a < 0.18f)
        return 0.0f;

    return a > 0.0f ? (a - 0.18f) / 0.82f : (a + 0.18f) / 0.82f;
}

static unsigned int g_rnd(void)
{
    gSeed = gSeed * 1664525u + 1013904223u;
    return gSeed >> 8;
}

/* ---- construção do cenário ---- */
static int g_clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Face retangular dividida em ladrilhos (origem o, arestas u e v). */
static void g_face(float ox, float oy, float oz,
                   float ux, float uy, float uz,
                   float vx, float vy, float vz,
                   float nx, float ny, float nz,
                   int r, int g, int b, int layer, int stain)
{
    float lu = g_sqrt(ux * ux + uy * uy + uz * uz);
    float lv = g_sqrt(vx * vx + vy * vy + vz * vz);
    int nu = (int)(lu / 1.5f) + 1;
    int nv = (int)(lv / 1.5f) + 1;
    int i, j, k;

    for (i = 0; i < nu; i++)
    {
        for (j = 0; j < nv; j++)
        {
            GQuad *q;
            float a0 = (float)i / nu, a1 = (float)(i + 1) / nu;
            float b0 = (float)j / nv, b1 = (float)(j + 1) / nv;
            int var = (int)(g_rnd() % 21u) - 10;
            int rr = r + var, gg = g + var, bb = b + var;

            if (gQuadN >= G_MAXQ)
                return;

            if (stain && (g_rnd() % 100u) < 7u)
            {
                rr = rr * 55 / 100 + 22;   /* mancha de sangue seco */
                gg = gg * 30 / 100;
                bb = bb * 30 / 100;
            }

            q = &gQuads[gQuadN++];
            for (k = 0; k < 4; k++)
            {
                float aa = (k == 0 || k == 3) ? a0 : a1;
                float bv = (k < 2) ? b0 : b1;

                q->c[k][0] = ox + ux * aa + vx * bv;
                q->c[k][1] = oy + uy * aa + vy * bv;
                q->c[k][2] = oz + uz * aa + vz * bv;
            }
            q->n[0] = nx; q->n[1] = ny; q->n[2] = nz;
            q->r = (unsigned char)g_clamp255(rr);
            q->g = (unsigned char)g_clamp255(gg);
            q->b = (unsigned char)g_clamp255(bb);
            q->layer = (unsigned char)layer;
        }
    }
}

static void g_floor(float x0, float z0, float x1, float z1)
{
    g_face(x0, 0.0f, z0,  x1 - x0, 0, 0,  0, 0, z1 - z0,  0, 1, 0, 58, 44, 32, 0, 0);
}

static void g_ceiling(float x0, float z0, float x1, float z1)
{
    g_face(x0, G_CEIL, z0,  x1 - x0, 0, 0,  0, 0, z1 - z0,  0, -1, 0, 70, 62, 54, 0, 0);
}

/* Caixa sólida ou decorativa. Material: 0 = reboco, 1 = madeira. */
static void g_box(float x0, float y0, float z0, float x1, float y1, float z1,
                  int material, int solid)
{
    int r, g, b, stain;
    float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;

    if (material == 0) { r = 150; g = 146; b = 136; stain = 1; }
    else               { r = 84;  g = 62;  b = 44;  stain = 0; }

    g_face(x0, y0, z0,  0, 0, dz,  0, dy, 0,  -1, 0, 0, r, g, b, 1, stain);   /* -x */
    g_face(x1, y0, z0,  0, 0, dz,  0, dy, 0,   1, 0, 0, r, g, b, 1, stain);   /* +x */
    g_face(x0, y0, z0,  dx, 0, 0,  0, dy, 0,   0, 0, -1, r, g, b, 1, stain);  /* -z */
    g_face(x0, y0, z1,  dx, 0, 0,  0, dy, 0,   0, 0, 1, r, g, b, 1, stain);   /* +z */

    if (y0 > 0.01f)
        g_face(x0, y0, z0,  dx, 0, 0,  0, 0, dz,  0, -1, 0, r, g, b, 1, 0);   /* baixo */
    if (y1 < G_CEIL - 0.01f)
        g_face(x0, y1, z0,  dx, 0, 0,  0, 0, dz,  0, 1, 0, r, g, b, 1, 0);    /* topo */

    if (solid && gBoxN < 96)
    {
        GBox *bx = &gBoxes[gBoxN++];
        bx->x0 = x0; bx->y0 = y0; bx->z0 = z0;
        bx->x1 = x1; bx->y1 = y1; bx->z1 = z1;
    }
}

static void g_table(float x0, float z0, float x1, float z1, float top)
{
    float t = 0.08f, l = 0.10f;

    g_box(x0, top - t, z0, x1, top, z1, 1, 1);
    g_box(x0, 0, z0, x0 + l, top - t, z0 + l, 1, 1);
    g_box(x1 - l, 0, z0, x1, top - t, z0 + l, 1, 1);
    g_box(x0, 0, z1 - l, x0 + l, top - t, z1, 1, 1);
    g_box(x1 - l, 0, z1 - l, x1, top - t, z1, 1, 1);
}

/* Cozinha, corredor e sala de jantar (provisório, só para testar). */
static void g_build_scene(void)
{
    gQuadN = 0;
    gBoxN = 0;
    gSeed = 12345u;

    /* chão e teto */
    g_floor(0, 0, 6, 4);        g_ceiling(0, 0, 6, 4);
    g_floor(6, 1.4f, 11, 2.6f); g_ceiling(6, 1.4f, 11, 2.6f);
    g_floor(11, 0, 15, 4);      g_ceiling(11, 0, 15, 4);

    /* cozinha */
    g_box(-0.2f, 0, -0.2f, 6.2f, G_CEIL, 0,    0, 1);   /* sul   */
    g_box(-0.2f, 0, 4,     6.2f, G_CEIL, 4.2f, 0, 1);   /* norte */
    g_box(-0.2f, 0, 0,     0,    G_CEIL, 4,    0, 1);   /* oeste */
    g_box(6, 0, 0,    6.2f, G_CEIL, 1.4f, 0, 1);        /* leste, lado sul da porta   */
    g_box(6, 0, 2.6f, 6.2f, G_CEIL, 4,    0, 1);        /* leste, lado norte da porta */
    g_box(6, 2.1f, 1.4f, 6.2f, G_CEIL, 2.6f, 0, 0);     /* verga da porta */

    /* corredor */
    g_box(6.2f, 0, 2.6f, 10.8f, G_CEIL, 2.8f, 0, 1);
    g_box(6.2f, 0, 1.2f, 10.8f, G_CEIL, 1.4f, 0, 1);

    /* sala de jantar */
    g_box(10.8f, 0, 0,    11, G_CEIL, 1.4f, 0, 1);
    g_box(10.8f, 0, 2.6f, 11, G_CEIL, 4,    0, 1);
    g_box(10.8f, 2.1f, 1.4f, 11, G_CEIL, 2.6f, 0, 0);
    g_box(10.8f, 0, -0.2f, 15.2f, G_CEIL, 0,    0, 1);
    g_box(10.8f, 0, 4,     15.2f, G_CEIL, 4.2f, 0, 1);
    g_box(15, 0, 0, 15.2f, G_CEIL, 4, 0, 1);

    /* móveis */
    g_box(0.3f, 0, 3.3f, 5.7f, 0.95f, 4, 1, 1);          /* balcão */
    g_box(0.3f, 1.5f, 3.7f, 3.2f, 2.2f, 4, 1, 0);        /* armário alto (enfeite) */
    g_table(2.0f, 1.0f, 4.2f, 1.9f, 1.12f);              /* ilha: dá para passar agachado */
    g_table(12.2f, 1.4f, 13.8f, 2.6f, 0.78f);            /* mesa de jantar */
    g_box(11.5f, 0, 1.6f, 11.95f, 0.5f, 2.05f, 1, 1);    /* cadeiras */
    g_box(14.05f, 0, 1.95f, 14.5f, 0.5f, 2.4f, 1, 1);

    gSceneBuilt = 1;
}

/* ---- colisão (círculo contra caixas, só no plano x-z) ---- */
static int g_blocked(float x, float z, float h)
{
    int i;

    for (i = 0; i < gBoxN; i++)
    {
        const GBox *b = &gBoxes[i];
        float cx, cz, dx, dz;

        if (b->y0 >= h)
            continue;

        cx = x < b->x0 ? b->x0 : (x > b->x1 ? b->x1 : x);
        cz = z < b->z0 ? b->z0 : (z > b->z1 ? b->z1 : z);
        dx = x - cx;
        dz = z - cz;

        if (dx * dx + dz * dz < G_RADIUS * G_RADIUS)
            return 1;
    }
    return 0;
}

static void g_collide(float *px, float *pz, float h)
{
    int pass, i;

    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < gBoxN; i++)
        {
            const GBox *b = &gBoxes[i];
            float cx, cz, dx, dz, d2;

            if (b->y0 >= h)
                continue;

            cx = *px < b->x0 ? b->x0 : (*px > b->x1 ? b->x1 : *px);
            cz = *pz < b->z0 ? b->z0 : (*pz > b->z1 ? b->z1 : *pz);
            dx = *px - cx;
            dz = *pz - cz;
            d2 = dx * dx + dz * dz;

            if (d2 >= G_RADIUS * G_RADIUS)
                continue;

            if (d2 > 1.0e-8f)
            {
                float d = g_sqrt(d2);
                float k = (G_RADIUS - d) / d;

                *px += dx * k;
                *pz += dz * k;
            }
            else
            {
                /* centro dentro da caixa: sai pelo lado mais curto */
                float m = *px - b->x0;
                float rr = b->x1 - *px, tt = *pz - b->z0, bb = b->z1 - *pz;
                int s = 0;

                if (rr < m) { m = rr; s = 1; }
                if (tt < m) { m = tt; s = 2; }
                if (bb < m) { m = bb; s = 3; }

                if (s == 0)      *px = b->x0 - G_RADIUS;
                else if (s == 1) *px = b->x1 + G_RADIUS;
                else if (s == 2) *pz = b->z0 - G_RADIUS;
                else             *pz = b->z1 + G_RADIUS;
            }
        }
    }
}

/* =====================================================================
   CASA REAL: carrega GRANNY/casa.bin (gerado a partir do modelo 3D).
   Desenha só os cômodos visíveis (lista pré-calculada) e só os pedaços
   que estão dentro da tela. Colisão e altura do chão vêm do arquivo.
   ===================================================================== */
#define H_STEP   0.32f     /* degrau máximo (m) */
#define H_ZN     0.25f     /* escala da profundidade (Z de 16 bits): evita triângulos brigando entre si */
#define H_CULL   0         /* 1 = descarta faces de costas (causava buracos pretos e teto transparente) */
#define H_FAR    22.0f
#define H_MAX_TRIS 12000   /* limite de triângulos por quadro (o que passa disso some = área preta) */

typedef struct { float mn[3], mx[3]; unsigned int a, b; } HBox;

static unsigned char *hData = NULL;
static int hLoaded = 0, hTried = 0, hCur = 0;
static unsigned int hNC = 0, hNCh = 0, hNT = 0;
static float hLevel[5];
static unsigned int hGW, hGH, hHW, hHH, hHK;
static float hGX0, hGZ0, hGS, hHX0, hHZ0, hHS;
static float hSpawn[4];
static const HBox *hCells = NULL, *hChunks = NULL;
static const short *hTris = NULL, *hHt = NULL;
static const unsigned char *hOcc = NULL, *hLab = NULL;
static unsigned int hPvs[32];
static float gFeet = 0.0f, gVY = 0.0f, gCamFeet = 0.0f;
static int   gStuckN = 0;         /* quadros seguidos empurrando sem sair do lugar */
static int   gDebug = 0;          /* 1 = painel de números (SELECT liga/desliga) */
static int   gDbgFlush = 0;       /* diagnóstico (só com o painel ligado): TRIÂNGULO troca de quantos em quantos triângulos esvazia a fila */
static int   gDbgZ = 0;           /* QUADRADO troca a escala da profundidade */
static int   gDbgPvs = 0;         /* L2 liga/desliga a lista de cômodos visíveis */
static int   gDbgCull = 0;        /* R2 liga/desliga o descarte de pedaços fora da tela */
static float hZN = 0.25f;
static const unsigned char *hVox = NULL;   /* tabela: quais pedaços do modelo se vê de cada trecho do chão */
static unsigned int hNV = 0;
static float hVXZ = 1.2f, hVY = 0.9f, hVY0 = -2.9f;
static int   hFlushN = 100;
#define HUD_Z 65535               /* HUD no Z máximo: passa no teste de profundidade em qualquer lugar */
static float gRawDt = 0.033f;     /* tempo real do último quadro (s) */
static float gFpsAvg = 30.0f;
static int hTriCount = 0;

static unsigned int rd_u(const unsigned char *p, int off) { unsigned int v; memcpy(&v, p + off, 4); return v; }
static float rd_f(const unsigned char *p, int off) { float v; memcpy(&v, p + off, 4); return v; }

static int house_load(const char *path)
{
    FILE *f;
    long sz;
    unsigned int offC, offCh, offT, offO, offL, offH, offP;
    int i;

    f = fopen(path, "rb");
    if (f == NULL)
        return 0;

    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz < 140)
    {
        fclose(f);
        return 0;
    }

    hData = (unsigned char *)malloc((size_t)sz);
    if (hData == NULL)
    {
        fclose(f);
        return 0;
    }

    if (fread(hData, 1, (size_t)sz, f) != (size_t)sz)
    {
        fclose(f);
        free(hData);
        hData = NULL;
        return 0;
    }
    fclose(f);

    if (memcmp(hData, "CASA", 4) != 0 || rd_u(hData, 4) != 1u)
    {
        free(hData);
        hData = NULL;
        return 0;
    }

    hNC = rd_u(hData, 8);  hNCh = rd_u(hData, 12);  hNT = rd_u(hData, 16);
    for (i = 0; i < 5; i++)
        hLevel[i] = rd_f(hData, 24 + 4 * i);
    hGW = rd_u(hData, 44);  hGH = rd_u(hData, 48);
    hGX0 = rd_f(hData, 52); hGZ0 = rd_f(hData, 56); hGS = rd_f(hData, 60);
    hHW = rd_u(hData, 64);  hHH = rd_u(hData, 68);
    hHX0 = rd_f(hData, 72); hHZ0 = rd_f(hData, 76); hHS = rd_f(hData, 80);
    hHK = rd_u(hData, 84);
    for (i = 0; i < 4; i++)
        hSpawn[i] = rd_f(hData, 88 + 4 * i);

    offC = rd_u(hData, 104);  offCh = rd_u(hData, 108);  offT = rd_u(hData, 112);
    offO = rd_u(hData, 116);  offL = rd_u(hData, 120);   offH = rd_u(hData, 124);
    offP = rd_u(hData, 128);

    if (hNC == 0 || hNC > 32 || offP + hNC * 4 > (unsigned int)sz)
    {
        free(hData);
        hData = NULL;
        return 0;
    }

    hCells  = (const HBox *)(hData + offC);
    hChunks = (const HBox *)(hData + offCh);
    hTris   = (const short *)(hData + offT);
    hOcc    = hData + offO;
    hLab    = hData + offL;
    hHt     = (const short *)(hData + offH);
    memset(hPvs, 0, sizeof(hPvs));
    for (i = 0; i < (int)hNC; i++)
        hPvs[i] = rd_u(hData, (int)offP + 4 * i);

    /* visibilidade por trecho do chão (opcional): deslocamento no cabeçalho, byte 132 */
    hVox = NULL; hNV = 0;
    {
        unsigned int offV = rd_u(hData, 132);

        if (offV != 0u && offV + 16u <= (unsigned int)sz)
        {
            unsigned int nv = rd_u(hData, (int)offV + 12);

            if (offV + 16u + nv * 72u <= (unsigned int)sz)
            {
                hVXZ = rd_f(hData, (int)offV);
                hVY  = rd_f(hData, (int)offV + 4);
                hVY0 = rd_f(hData, (int)offV + 8);
                hNV  = nv;
                hVox = hData + offV + 16;
            }
        }
    }

    hLoaded = 1;
    return 1;
}

static int g_floori(float v)
{
    int i = (int)v;

    return (v < (float)i) ? i - 1 : i;
}

/* Tabela de visibilidade do trecho onde o jogador está (NULL se não houver). */
static const unsigned char *h_find_vox(void)
{
    int ix, iz, iy;
    unsigned int i;

    if (hVox == NULL || hVXZ <= 0.0f || hVY <= 0.0f)
        return NULL;

    ix = g_floori(gPX / hVXZ);
    iz = g_floori(gPZ / hVXZ);
    iy = g_floori((gFeet - hVY0) / hVY);

    for (i = 0; i < hNV; i++)
    {
        const unsigned char *r = hVox + (size_t)i * 72;
        int a = (short)(r[0] | (r[1] << 8));
        int b = (short)(r[2] | (r[3] << 8));
        int c = (short)(r[4] | (r[5] << 8));

        if (a == ix && b == iz && c == iy)
            return r + 8;
    }
    return NULL;
}

static int h_level(float feet)
{
    int k, l = 0;

    for (k = 0; k < 5; k++)
        if (hLevel[k] <= feet + 0.45f)
            l = k;
    return l;
}

static int h_occ_at(int lv, float x, float z)
{
    int gx, gz;

    if (x < hGX0 || z < hGZ0)
        return 1;
    gx = (int)((x - hGX0) / hGS);
    gz = (int)((z - hGZ0) / hGS);
    if (gx >= (int)hGW || gz >= (int)hGH)
        return 1;
    return hOcc[(size_t)lv * hGW * hGH + (size_t)gz * hGW + (size_t)gx];
}

static int h_wall_hit(int lv, float x, float z)
{
    int rc = (int)(G_RADIUS / hGS) + 1, dx, dz;

    for (dz = -rc; dz <= rc; dz++)
    {
        for (dx = -rc; dx <= rc; dx++)
        {
            float fx = (float)dx * hGS, fz = (float)dz * hGS;

            if (fx * fx + fz * fz > G_RADIUS * G_RADIUS)
                continue;
            if (h_occ_at(lv, x + fx, z + fz))
                return 1;
        }
    }
    return 0;
}

/* 1 se dá para ficar em (x,z); *floorY = altura do chão ali. */
static int h_floor(float x, float z, float feet, float headH, float *floorY)
{
    int hx, hz, k;
    const short *L;
    float best = -1.0e9f;

    if (x < hHX0 || z < hHZ0)
        return 0;
    hx = (int)((x - hHX0) / hHS);
    hz = (int)((z - hHZ0) / hHS);
    if (hx >= (int)hHW || hz >= (int)hHH)
        return 0;

    L = hHt + ((size_t)hz * hHW + (size_t)hx) * hHK;

    for (k = 0; k < (int)hHK; k++)
    {
        float y;
        if (L[k] == -32768) break;
        y = (float)L[k] / 256.0f;
        if (y <= feet + H_STEP && y > best)
            best = y;
    }
    if (best < -1.0e8f)
        return 0;

    for (k = 0; k < (int)hHK; k++)
    {
        float y;
        if (L[k] == -32768) break;
        y = (float)L[k] / 256.0f;
        if (y > best + H_STEP && y < best + headH)
            return 0;                     /* bate a cabeça */
    }

    *floorY = best;
    return 1;
}

static int h_find_cell(void)
{
    int lv = h_level(gFeet), r, dx, dz;
    int gx, gz;

    if (gPX < hGX0 || gPZ < hGZ0)
        return hCur;
    gx = (int)((gPX - hGX0) / hGS);
    gz = (int)((gPZ - hGZ0) / hGS);

    for (r = 0; r <= 8; r++)
    {
        for (dz = -r; dz <= r; dz++)
        {
            for (dx = -r; dx <= r; dx++)
            {
                int x = gx + dx, z = gz + dz;

                if (r > 0 && dx > -r && dx < r && dz > -r && dz < r)
                    continue;
                if (x < 0 || z < 0 || x >= (int)hGW || z >= (int)hGH)
                    continue;
                if (hLab[(size_t)lv * hGW * hGH + (size_t)z * hGW + (size_t)x])
                    return (int)hLab[(size_t)lv * hGW * hGH + (size_t)z * hGW + (size_t)x] - 1;
            }
        }
    }
    return hCur;
}

/* Parede considerando a altura dos pés. Perto do piso de um andar vale a
   grade daquele andar. ENTRE dois andares (na escada) só vale a parede que
   existe nos DOIS andares: antes a grade trocava no meio dos degraus e
   prendia o jogador no penúltimo degrau. */
static int h_blocked(float feet, float x, float z)
{
    int low = 0, k;

    for (k = 0; k < 5; k++)
        if (hLevel[k] <= feet + 0.12f)
            low = k;
    if (low >= 4 || feet - hLevel[low] < 0.12f)
        return h_wall_hit(low, x, z);
    return h_wall_hit(low, x, z) && h_wall_hit(low + 1, x, z);
}

static void house_move(float dx, float dz, float headH, float dt)
{
    float fy = 0.0f;
    /* se já está dentro de uma parede (raro), deixa sair andando */
    int emb = h_blocked(gFeet, gPX, gPZ);

    if (dx != 0.0f)
    {
        float nx = gPX + dx;
        if ((emb || !h_blocked(gFeet, nx, gPZ)) && h_floor(nx, gPZ, gFeet, headH, &fy))
            gPX = nx;
    }
    if (dz != 0.0f)
    {
        float nz = gPZ + dz;
        if ((emb || !h_blocked(gFeet, gPX, nz)) && h_floor(gPX, nz, gFeet, headH, &fy))
            gPZ = nz;
    }

    if (h_floor(gPX, gPZ, gFeet, headH, &fy))
    {
        if (fy >= gFeet - 0.001f)
        {
            gFeet = fy;
            gVY = 0.0f;
        }
        else
        {
            gVY -= 9.8f * dt;
            gFeet += gVY * dt;
            if (gFeet < fy)
            {
                gFeet = fy;
                gVY = 0.0f;
            }
        }
    }

    hCur = h_find_cell();
}

static int h_can_stand(void)
{
    float fy = 0.0f;
    return h_floor(gPX, gPZ, gFeet, G_STAND_H, &fy);
}

static void game_start(void)
{
    if (!hTried)
    {
        hTried = 1;
        house_load("mass:/GRANNY/casa.bin");
    }

    if (hLoaded)
    {
        gPX = hSpawn[0];
        gFeet = hSpawn[1];
        gPZ = hSpawn[2];
        gCamFeet = gFeet;
        gVY = 0.0f;
        gYaw = hSpawn[3];
        hCur = 0;
        hCur = h_find_cell();
    }
    else
    {
        if (!gSceneBuilt)
            g_build_scene();

        gPX = 0.9f;
        gPZ = 2.0f;
        gYaw = G_PI * 0.5f;      /* olhando para a porta (leste) */
    }
    gPitch = 0.0f;
    gCrouch = 0;
    gCrouchReq = 0;
    gExitReq = 0;
    gEye = G_EYE_STAND;
    gLastTick = cpu_ticks_now();
}

/* Um passo da simulação (dt em segundos). */
static void game_step(float dt)
{
    float fwd = -g_axis(gPadLY), str = g_axis(gPadLX);
    float ryaw = g_axis(gPadRX), rpit = g_axis(gPadRY);
    float fx, fz, rx, rz, vx, vz, len, speed, h, target, k;

    if (gPadLX == 128 && gPadLY == 128)       /* sem analógico: direcional */
    {
        if (gPadHeld & PAD_UP)    fwd += 1.0f;
        if (gPadHeld & PAD_DOWN)  fwd -= 1.0f;
        if (gPadHeld & PAD_RIGHT) str += 1.0f;
        if (gPadHeld & PAD_LEFT)  str -= 1.0f;
    }
    if (gPadHeld & PAD_R1) ryaw += 1.0f;
    if (gPadHeld & PAD_L1) ryaw -= 1.0f;

    gYaw   += ryaw * 2.6f * dt;
    gPitch -= rpit * 1.8f * dt;
    if (gPitch > 1.2f)  gPitch = 1.2f;
    if (gPitch < -1.2f) gPitch = -1.2f;

    /* agachar / levantar (não levanta se bater a cabeça) */
    if (gCrouchReq)
    {
        gCrouchReq = 0;
        if (!gCrouch)
            gCrouch = 1;
        else if (hLoaded ? h_can_stand() : !g_blocked(gPX, gPZ, G_STAND_H))
            gCrouch = 0;
    }

    h = gCrouch ? G_CROUCH_H : G_STAND_H;
    speed = gCrouch ? 1.1f : 2.3f;

    fx = g_sin(gYaw);  fz = g_cos(gYaw);
    rx = g_cos(gYaw);  rz = -g_sin(gYaw);
    vx = fx * fwd + rx * str;
    vz = fz * fwd + rz * str;
    len = g_sqrt(vx * vx + vz * vz);
    if (len > 1.0f)
    {
        vx /= len;
        vz /= len;
    }

    if (hLoaded)
    {
        {
            float ox = gPX, oz = gPZ, want = len * speed * dt, nrst = 1.0e9f, mv;
            int kq;

            house_move(vx * speed * dt, vz * speed * dt, h, dt);

            for (kq = 0; kq < 5; kq++)
            {
                float dd = gFeet - hLevel[kq];
                if (dd < 0.0f) dd = -dd;
                if (dd < nrst) nrst = dd;
            }
            mv = g_sqrt((gPX - ox) * (gPX - ox) + (gPZ - oz) * (gPZ - oz));
            /* só vale na escada (fora do piso de um andar) */
            if (want > 0.01f && mv < want * 0.25f && nrst >= 0.30f)
                gStuckN++;
            else
                gStuckN = 0;
        }
        k = dt * 12.0f;
        if (k > 1.0f) k = 1.0f;
        gCamFeet += (gFeet - gCamFeet) * k;
    }
    else
    {
        gPX += vx * speed * dt;
        gPZ += vz * speed * dt;
        g_collide(&gPX, &gPZ, h);
    }

    target = gCrouch ? G_EYE_CROUCH : G_EYE_STAND;
    k = dt * 10.0f;
    if (k > 1.0f) k = 1.0f;
    gEye += (target - gEye) * k;
}

/* ---- desenho ---- */
static int gcmp(const void *a, const void *b)
{
    float ka = ((const GSortItem *)a)->key;
    float kb = ((const GSortItem *)b)->key;

    return (ka < kb) - (ka > kb);          /* mais longe primeiro */
}

static int g_clip(const GV *in, int n, GV *out, float a, float b, float c, float d)
{
    int i, m = 0;

    for (i = 0; i < n; i++)
    {
        const GV *p = &in[i];
        const GV *q = &in[(i + 1) % n];
        float dp = a * p->x + b * p->y + c * p->z + d;
        float dq = a * q->x + b * q->y + c * q->z + d;

        if (dp >= 0.0f)
            out[m++] = *p;

        if ((dp >= 0.0f) != (dq >= 0.0f))
        {
            float t = dp / (dp - dq);

            out[m].x = p->x + (q->x - p->x) * t;
            out[m].y = p->y + (q->y - p->y) * t;
            out[m].z = p->z + (q->z - p->z) * t;
            out[m].b = p->b + (q->b - p->b) * t;
            m++;
        }
    }
    return m;
}

static u64 g_color(const GQuad *q, float k)
{
    int r = (int)((float)q->r * k);
    int g = (int)((float)q->g * k);
    int b = (int)((float)q->b * k);

    return GS_SETREG_RGBAQ(g_clamp255(r), g_clamp255(g), g_clamp255(b), 0x80, 0);
}

/* visibilidade de uma caixa (cantos em coordenadas do mundo) */
static int h_box_visible(const float *mn, const float *mx, const float *cam,
                         const float *Rv, const float *Uv, const float *Fv,
                         float kx, float ky)
{
    int i, nl = 0, nr = 0, nb = 0, nt = 0, nn = 0, nf = 0;

    for (i = 0; i < 8; i++)
    {
        float dx = ((i & 1) ? mx[0] : mn[0]) - cam[0];
        float dy = ((i & 2) ? mx[1] : mn[1]) - cam[1];
        float dz = ((i & 4) ? mx[2] : mn[2]) - cam[2];
        float x = dx * Rv[0] + dy * Rv[1] + dz * Rv[2];
        float y = dx * Uv[0] + dy * Uv[1] + dz * Uv[2];
        float z = dx * Fv[0] + dy * Fv[1] + dz * Fv[2];

        if (z < G_NEAR)      nn++;
        if (z > H_FAR)       nf++;
        if (x < -kx * z)     nl++;
        if (x >  kx * z)     nr++;
        if (y < -ky * z)     nb++;
        if (y >  ky * z)     nt++;
    }
    return !(nn == 8 || nf == 8 || nl == 8 || nr == 8 || nb == 8 || nt == 8);
}

static void h_emit(GSGLOBAL *gs, const float *sx, const float *sy, const float *z,
                   const float *br, int r, int g, int b)
{
    int iz[3], i;
    u64 c[3];

    for (i = 0; i < 3; i++)
    {
        float zz = z[i] < hZN ? hZN : z[i];
        int zi = (int)(65535.0f * hZN / zz);          /* H_ZN maior = mais precisão de profundidade longe da câmera */

        iz[i] = zi < 1 ? 1 : (zi > 65535 ? 65535 : zi);
        c[i] = GS_SETREG_RGBAQ(g_clamp255((int)((float)r * br[i])),
                               g_clamp255((int)((float)g * br[i])),
                               g_clamp255((int)((float)b * br[i])), 0x80, 0);
    }

    gsKit_prim_triangle_gouraud_3d(gs, sx[0], sy[0], iz[0], sx[1], sy[1], iz[1],
                                   sx[2], sy[2], iz[2], c[0], c[1], c[2]);

    hTriCount++;
    if (hFlushN > 0 && (hTriCount % hFlushN) == 0)
        gsKit_queue_exec(gs);
}

/* Cômodos que podem ser vistos. Na escada o cômodo "atual" pelo mapa do
   andar fica errado (e a tela ficava com buracos pretos); então, fora de um
   piso de andar, junta a visibilidade de todos os cômodos cujo volume
   contém a câmera. */
static unsigned int h_pvs_mask(float eyeY)
{
    unsigned int m = hPvs[hCur];
    float nearest = 1.0e9f;
    int k, c;

    for (k = 0; k < 5; k++)
    {
        float dd = gFeet - hLevel[k];
        if (dd < 0.0f) dd = -dd;
        if (dd < nearest) nearest = dd;
    }
    if (nearest < 0.30f)
        return m;

    for (c = 0; c < (int)hNC; c++)
    {
        const HBox *b = &hCells[c];
        float my = gFeet + 0.9f;

        if (gPX < b->mn[0] - 0.3f || gPX > b->mx[0] + 0.3f ||
            gPZ < b->mn[2] - 0.3f || gPZ > b->mx[2] + 0.3f)
            continue;
        if ((eyeY >= b->mn[1] - 0.3f && eyeY <= b->mx[1] + 0.3f) ||
            (my   >= b->mn[1] - 0.3f && my   <= b->mx[1] + 0.3f))
            m |= hPvs[c];
    }
    return m;
}

static void house_draw(GSGLOBAL *gs, int brightness)
{
    float W = (float)gs->Width, H = (float)gs->Height;
    float focal = W * 0.60f;
    float kx = (W * 0.5f + 48.0f) / focal, ky = (H * 0.5f + 48.0f) / focal;
    float kxe = (W * 0.5f) / focal, kye = (H * 0.5f) / focal;
    float bscale = 0.45f + 0.055f * (float)brightness;
    float sy_ = g_sin(gYaw), cy_ = g_cos(gYaw), sp = g_sin(gPitch), cp = g_cos(gPitch);
    float Fv[3], Rv[3], Uv[3], cam[3];
    unsigned int mask;
    const unsigned char *vb;
    int c, j, i;
    const float inv = 1.0f / 256.0f;

    Fv[0] = sy_ * cp; Fv[1] = sp;   Fv[2] = cy_ * cp;
    Rv[0] = cy_;      Rv[1] = 0.0f; Rv[2] = -sy_;
    Uv[0] = -sy_ * sp; Uv[1] = cp;  Uv[2] = -cy_ * sp;
    cam[0] = gPX; cam[1] = gCamFeet + gEye; cam[2] = gPZ;

    /* limpa cor e Z (o desenho com teste de Z desligado grava Z = 0) */
    gsKit_set_test(gs, GS_ZTEST_OFF);
    gsKit_prim_sprite(gs, 0.0f, 0.0f, W, H, 0,
                     gDebug ? GS_SETREG_RGBAQ(255, 0, 255, 0x80, 0) : GS_SETREG_RGBAQ(4, 4, 6, 0x80, 0));
    gsKit_set_test(gs, GS_ZTEST_ON);

    hTriCount = 0;
    hZN = gDbgZ == 0 ? 0.25f : (gDbgZ == 1 ? 0.10f : 0.50f);
    hFlushN = gDbgFlush == 0 ? 100 : (gDbgFlush == 1 ? 25 : 400);
    vb = gDbgPvs ? NULL : h_find_vox();
    mask = (gDbgPvs || hVox != NULL) ? 0xFFFFFFFFu : h_pvs_mask(cam[1]);

    {
        int ord[32], no = 0, q, w;
        float d2c[32];

        for (c = 0; c < (int)hNC; c++)
        {
            const HBox *cb = &hCells[c];
            float mx_ = (cb->mn[0] + cb->mx[0]) * 0.5f - cam[0];
            float mz_ = (cb->mn[2] + cb->mx[2]) * 0.5f - cam[2];

            if (!((mask >> c) & 1u) || cb->b == 0)
                continue;
            if (!h_box_visible(cb->mn, cb->mx, cam, Rv, Uv, Fv, kxe, kye))
                continue;

            d2c[no] = (c == hCur) ? -1.0f : mx_ * mx_ + mz_ * mz_;
            ord[no++] = c;
        }

        for (q = 1; q < no; q++)             /* mais perto primeiro */
        {
            int oc = ord[q];
            float od = d2c[q];

            for (w = q - 1; w >= 0 && d2c[w] > od; w--)
            {
                ord[w + 1] = ord[w];
                d2c[w + 1] = d2c[w];
            }
            ord[w + 1] = oc;
            d2c[w + 1] = od;
        }

    for (q = 0; q < no; q++)
    {
        const HBox *cb = &hCells[ord[q]];

        for (j = (int)cb->a; j < (int)(cb->a + cb->b); j++)
        {
            const HBox *ch = &hChunks[j];

            if (vb != NULL && !((vb[j >> 3] >> (j & 7)) & 1))
                continue;
            if (!gDbgCull && !h_box_visible(ch->mn, ch->mx, cam, Rv, Uv, Fv, kxe, kye))
                continue;
            if (hTriCount + (int)ch->b > H_MAX_TRIS)
                continue;

            for (i = (int)ch->a; i < (int)(ch->a + ch->b); i++)
            {
                const short *t = hTris + (size_t)i * 11;
                GV v[3];
                int k, out = 0, behind = 0, allfar = 0;
                int r = t[9] & 255, g = (t[9] >> 8) & 255, b = t[10] & 255;
                float d2[3];

                for (k = 0; k < 3; k++)
                {
                    float dx = (float)t[3 * k] * inv - cam[0];
                    float dy = (float)t[3 * k + 1] * inv - cam[1];
                    float dz = (float)t[3 * k + 2] * inv - cam[2];

                    v[k].x = dx * Rv[0] + dy * Rv[1] + dz * Rv[2];
                    v[k].y = dx * Uv[0] + dy * Uv[1] + dz * Uv[2];
                    v[k].z = dx * Fv[0] + dy * Fv[1] + dz * Fv[2];
                    d2[k] = dx * dx + dy * dy + dz * dz;
                    v[k].b = bscale / (1.0f + 0.025f * d2[k]);

                    if (v[k].z < G_NEAR) behind++;
                    if (v[k].z > H_FAR)  allfar++;
                    if (v[k].x < -kx * v[k].z) out |= 1;
                    if (v[k].x >  kx * v[k].z) out |= 2;
                    if (v[k].y < -ky * v[k].z) out |= 4;
                    if (v[k].y >  ky * v[k].z) out |= 8;
                }

                if (behind == 3 || allfar == 3)
                    continue;

                /* face de um lado só: descarta se a câmera está do lado de trás.
                   DESLIGADO (H_CULL 0): em triângulos finos o teste erra por causa do
                   arredondamento das coordenadas e some pedaço de chão/teto. */
                if (H_CULL && ((t[10] >> 8) & 3))
                {
                    float e1x = v[1].x - v[0].x, e1y = v[1].y - v[0].y, e1z = v[1].z - v[0].z;
                    float e2x = v[2].x - v[0].x, e2y = v[2].y - v[0].y, e2z = v[2].z - v[0].z;
                    float dp = (e1y * e2z - e1z * e2y) * v[0].x +
                               (e1z * e2x - e1x * e2z) * v[0].y +
                               (e1x * e2y - e1y * e2x) * v[0].z;

                    if ((((t[10] >> 8) & 3) == 1 && dp > 0.0f) ||
                        (((t[10] >> 8) & 3) == 2 && dp < 0.0f))
                        continue;
                }

                if (behind == 0 && out == 0)
                {
                    float sx[3], sy[3], zz[3], br[3];

                    for (k = 0; k < 3; k++)
                    {
                        sx[k] = W * 0.5f + v[k].x * focal / v[k].z;
                        sy[k] = H * 0.5f - v[k].y * focal / v[k].z;
                        zz[k] = v[k].z;
                        br[k] = v[k].b;
                    }
                    h_emit(gs, sx, sy, zz, br, r, g, b);
                }
                else
                {
                    GV a[12], bb[12];
                    int m = 3, n;

                    a[0] = v[0]; a[1] = v[1]; a[2] = v[2];
                    m = g_clip(a, m, bb, 0, 0, 1, -G_NEAR);   if (m < 3) continue;
                    m = g_clip(bb, m, a, 1, 0, kx, 0);        if (m < 3) continue;
                    m = g_clip(a, m, bb, -1, 0, kx, 0);       if (m < 3) continue;
                    m = g_clip(bb, m, a, 0, 1, ky, 0);        if (m < 3) continue;
                    m = g_clip(a, m, bb, 0, -1, ky, 0);       if (m < 3) continue;

                    for (n = 1; n + 1 < m; n++)
                    {
                        float sx[3], sy[3], zz[3], br[3];
                        const GV *p3[3];

                        p3[0] = &bb[0]; p3[1] = &bb[n]; p3[2] = &bb[n + 1];
                        for (k = 0; k < 3; k++)
                        {
                            sx[k] = W * 0.5f + p3[k]->x * focal / p3[k]->z;
                            sy[k] = H * 0.5f - p3[k]->y * focal / p3[k]->z;
                            zz[k] = p3[k]->z;
                            br[k] = p3[k]->b;
                        }
                        h_emit(gs, sx, sy, zz, br, r, g, b);
                    }
                }
            }
        }
    }

    }

    gsKit_set_test(gs, GS_ZTEST_OFF);        /* HUD por cima de tudo */
}

static GSTEXTURE hudTex;
static int hudOk = 0, hudTried = 0;

/* ---- painel de números para diagnóstico (SELECT liga/desliga) ---- */
static void dbg_digit(GSGLOBAL *gs, float x, float y, int d, u64 col)
{
    static const unsigned char seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    unsigned char m = seg[d];
    const float w = 8.0f, h = 7.0f, t = 2.0f;

    if (m & 1)  gsKit_prim_sprite(gs, x, y, x + w, y + t, HUD_Z, col);
    if (m & 2)  gsKit_prim_sprite(gs, x + w - t, y, x + w, y + h, HUD_Z, col);
    if (m & 4)  gsKit_prim_sprite(gs, x + w - t, y + h, x + w, y + 2 * h, HUD_Z, col);
    if (m & 8)  gsKit_prim_sprite(gs, x, y + 2 * h - t, x + w, y + 2 * h, HUD_Z, col);
    if (m & 16) gsKit_prim_sprite(gs, x, y + h, x + t, y + 2 * h, HUD_Z, col);
    if (m & 32) gsKit_prim_sprite(gs, x, y, x + t, y + h, HUD_Z, col);
    if (m & 64) gsKit_prim_sprite(gs, x, y + h - 1.0f, x + w, y + h + 1.0f, HUD_Z, col);
}

/* linha: quadrado colorido (legenda) + número com 'nd' dígitos; negativo = barra na frente */
static void dbg_row(GSGLOBAL *gs, float x, float y, u64 tag, int value, int nd)
{
    u64 yel = GS_SETREG_RGBAQ(255, 235, 60, 0x80, 0);
    u64 blk = GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0);
    int i, neg = value < 0, d[8];

    if (neg) value = -value;
    for (i = nd - 1; i >= 0; i--) { d[i] = value % 10; value /= 10; }

    gsKit_prim_sprite(gs, x - 4, y - 3, x + 18 + nd * 12 + 6, y + 19, HUD_Z, blk);
    gsKit_prim_sprite(gs, x, y + 2, x + 10, y + 12, HUD_Z, tag);
    if (neg) gsKit_prim_sprite(gs, x + 13, y + 6, x + 19, y + 8, HUD_Z, yel);
    for (i = 0; i < nd; i++)
        dbg_digit(gs, x + 22 + i * 12, y, d[i], yel);
}

static void dbg_overlay(GSGLOBAL *gs, float H)
{
    float k = H / 448.0f, x = 70.0f * k, y = H - 180.0f * k;
    float fps = gRawDt > 0.0001f ? 1.0f / gRawDt : 99.0f;

    gFpsAvg += (fps - gFpsAvg) * 0.1f;
    dbg_row(gs, x, y,            GS_SETREG_RGBAQ(40, 220, 40, 0x80, 0),   (int)(gFpsAvg + 0.5f), 2);   /* verde: quadros por segundo */
    dbg_row(gs, x, y + 24 * k,   GS_SETREG_RGBAQ(70, 120, 255, 0x80, 0),  hTriCount, 5);               /* azul: triângulos desenhados */
    dbg_row(gs, x, y + 48 * k,   GS_SETREG_RGBAQ(230, 40, 40, 0x80, 0),   hCur, 2);                    /* vermelho: cômodo atual */
    dbg_row(gs, x, y + 72 * k,   GS_SETREG_RGBAQ(255, 255, 255, 0x80, 0), (int)(gFeet * 10.0f), 3);    /* branco: altura dos pés x10 */
    dbg_row(gs, x, y + 120 * k,  GS_SETREG_RGBAQ(255, 150, 0, 0x80, 0),   gDbgFlush * 1000 + gDbgZ * 100 + gDbgPvs * 10 + gDbgCull, 4); /* laranja: modos de teste */
    dbg_row(gs, x, y + 96 * k,   GS_SETREG_RGBAQ(230, 60, 230, 0x80, 0),  gStuckN, 3);   /* rosa: quadros seguidos empurrando sem sair do lugar na escada */
}

static void game_draw(GSGLOBAL *gs, int brightness)
{
    static GSortItem order[G_MAXQ];
    float W = (float)gs->Width, H = (float)gs->Height;
    float focal = W * 0.60f;
    float kx = (W * 0.5f + 48.0f) / focal, ky = (H * 0.5f + 48.0f) / focal;
    float bscale = (40.0f + 12.0f * (float)brightness) / 100.0f;
    float sy_ = g_sin(gYaw), cy_ = g_cos(gYaw);
    float sp = g_sin(gPitch), cp = g_cos(gPitch);
    float Fx = sy_ * cp, Fy = sp,  Fz = cy_ * cp;       /* frente  */
    float Rx = cy_,      Ry = 0.0f, Rz = -sy_;          /* direita */
    float Ux = -sy_ * sp, Uy = cp,  Uz = -cy_ * sp;     /* cima    */
    float cx = gPX, cy = gEye, cz = gPZ;
    int i, n = 0, tris = 0;

    if (hLoaded)
    {
        house_draw(gs, brightness);
        goto hud;
    }

    /* 1) escolhe as faces voltadas para a câmera e mede a distância */
    for (i = 0; i < gQuadN; i++)
    {
        const GQuad *q = &gQuads[i];
        float mx = (q->c[0][0] + q->c[2][0]) * 0.5f - cx;
        float my = (q->c[0][1] + q->c[2][1]) * 0.5f - cy;
        float mz = (q->c[0][2] + q->c[2][2]) * 0.5f - cz;

        if (mx * q->n[0] + my * q->n[1] + mz * q->n[2] >= 0.0f)
            continue;                                   /* de costas */

        if (mx * Fx + my * Fy + mz * Fz < -3.0f)
            continue;                                   /* bem atrás de nós */

        order[n].key = (float)q->layer * -100000.0f + (mx * mx + my * my + mz * mz);
        order[n].idx = i;
        n++;
    }

    /* camada 0 (chão/teto) sempre primeiro; depois do mais longe ao mais perto */
    for (i = 0; i < n; i++)
        if (gQuads[order[i].idx].layer == 0)
            order[i].key = 1.0e9f;

    qsort(order, (size_t)n, sizeof(GSortItem), gcmp);

    /* 2) projeta e desenha */
    for (i = 0; i < n; i++)
    {
        const GQuad *q = &gQuads[order[i].idx];
        GV a[12], b[12];
        float sx[12], sy[12];
        int k, m = 4;

        for (k = 0; k < 4; k++)
        {
            float dx = q->c[k][0] - cx, dy = q->c[k][1] - cy, dz = q->c[k][2] - cz;

            a[k].x = dx * Rx + dy * Ry + dz * Rz;
            a[k].y = dx * Ux + dy * Uy + dz * Uz;
            a[k].z = dx * Fx + dy * Fy + dz * Fz;
            a[k].b = bscale / (1.0f + 0.05f * (dx * dx + dy * dy + dz * dz));
        }

        m = g_clip(a, m, b, 0, 0, 1, -G_NEAR);   if (m < 3) continue;
        m = g_clip(b, m, a, 1, 0, kx, 0);        if (m < 3) continue;
        m = g_clip(a, m, b, -1, 0, kx, 0);       if (m < 3) continue;
        m = g_clip(b, m, a, 0, 1, ky, 0);        if (m < 3) continue;
        m = g_clip(a, m, b, 0, -1, ky, 0);       if (m < 3) continue;

        for (k = 0; k < m; k++)
        {
            sx[k] = W * 0.5f + b[k].x * focal / b[k].z;
            sy[k] = H * 0.5f - b[k].y * focal / b[k].z;
        }

        for (k = 1; k + 1 < m; k++)
        {
            gsKit_prim_triangle_gouraud(gs,
                sx[0], sy[0], sx[k], sy[k], sx[k + 1], sy[k + 1], 1,
                g_color(q, b[0].b), g_color(q, b[k].b), g_color(q, b[k + 1].b));

            tris++;
            if (G_FLUSH_TRIS > 0 && (tris % G_FLUSH_TRIS) == 0)
                gsKit_queue_exec(gs);
        }
    }

    /* 3) HUD: ponto central e silhueta (em pé / agachado).
       Branco com contorno preto, para aparecer sobre paredes claras, e longe
       da borda, porque a TV corta as pontas da imagem. */
hud:
    /* HUD em imagem com fundo transparente: GRANNY/hud.png (144x48: mira | em pé | agachado) */
    if (!hudTried)
    {
        hudTried = 1;
        hudOk = load_png(gs, &hudTex, "hud");
    }
    if (hudOk)
    {
        u64 col = GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0);

        gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
        gs->PrimAlphaEnable = GS_SETTING_ON;
        draw_sub(gs, &hudTex, 320.0f - 24.0f, 224.0f - 24.0f, 48.0f, 48.0f, 0.0f, 0.0f, col, HUD_Z);
        draw_sub(gs, &hudTex, 640.0f - 120.0f, 448.0f - 112.0f, 48.0f, 48.0f,
                 gCrouch ? 96.0f : 48.0f, 0.0f, col, HUD_Z);
        gs->PrimAlphaEnable = GS_SETTING_OFF;

        if (gDebug && hLoaded)
            dbg_overlay(gs, H);
        return;
    }
    {
        u64 white = GS_SETREG_RGBAQ(255, 255, 255, 0x80, 0);
        u64 dark  = GS_SETREG_RGBAQ(0, 0, 0, 0x80, 0);
        float k  = H / 448.0f;            /* escala se a tela não for 448 de altura */
        float hx = 78.0f * k, hy = 96.0f * k;
        float cx = W * 0.5f, cy = H * 0.5f;
        float r[4][4];
        int n, i;

        if (!gCrouch)
        {
            n = 3;
            r[0][0] = -6; r[0][1] = -24; r[0][2] =  6; r[0][3] = -12;   /* cabeça */
            r[1][0] = -10; r[1][1] = -11; r[1][2] = 10; r[1][3] = 18;   /* corpo  */
            r[2][0] = -8; r[2][1] =  19; r[2][2] =  8; r[2][3] = 36;    /* pernas */
        }
        else
        {
            n = 3;
            r[0][0] = -6; r[0][1] = -8;  r[0][2] =  6; r[0][3] =  4;
            r[1][0] = -10; r[1][1] =  5; r[1][2] = 10; r[1][3] = 22;
            r[2][0] = -11; r[2][1] = 23; r[2][2] = 11; r[2][3] = 34;
        }

        /* mira: cruz branca com contorno preto */
        gsKit_prim_sprite(gs, cx - 7.0f, cy - 2.0f, cx + 7.0f, cy + 2.0f, HUD_Z, dark);
        gsKit_prim_sprite(gs, cx - 2.0f, cy - 7.0f, cx + 2.0f, cy + 7.0f, HUD_Z, dark);
        gsKit_prim_sprite(gs, cx - 5.0f, cy - 1.0f, cx + 5.0f, cy + 1.0f, HUD_Z, white);
        gsKit_prim_sprite(gs, cx - 1.0f, cy - 5.0f, cx + 1.0f, cy + 5.0f, HUD_Z, white);

        if (gDebug && hLoaded)
            dbg_overlay(gs, H);

        /* silhueta: primeiro todos os contornos, depois o branco */
        for (i = 0; i < n; i++)
            gsKit_prim_sprite(gs, hx + (r[i][0] - 2.0f) * k, hy + (r[i][1] - 2.0f) * k,
                                  hx + (r[i][2] + 2.0f) * k, hy + (r[i][3] + 2.0f) * k, HUD_Z, dark);
        for (i = 0; i < n; i++)
            gsKit_prim_sprite(gs, hx + r[i][0] * k, hy + r[i][1] * k,
                                  hx + r[i][2] * k, hy + r[i][3] * k, HUD_Z, white);
    }
}

int main(int argc, char *argv[])
{
    GSGLOBAL *gsGlobal;
    int screen = SCREEN_MENU;
    int optSel = 0;
    int multiSel = 0;       /* 0=2 jogadores, 1=3, 2=4 */
    int devPopup = 0;       /* 1 = mostrando "em desenvolvimento" */
    SaveData sd;            /* preferências + dados da partida */
    int playSel = PI_DIFF;  /* item selecionado na tela de JOGAR */
    int playPopup = 0;      /* 1 = em desenvolvimento, 2 = salvo, 3 = falha */
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

    /* Memory card (save principal) e dados salvos. */
    save_defaults(&sd);
    mcReady = mc_start();
    config_load(&sd);
    set_volumes(sd.musicVol, sd.masterVol);

    gsGlobal = gsKit_init_global();

    gsGlobal->PSM = GS_PSM_CT24;
    gsGlobal->PSMZ = GS_PSMZ_16S;
    gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;
    gsGlobal->ZBuffering = GS_SETTING_ON;    /* usado só na cena 3D */

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
    gsKit_set_test(gsGlobal, GS_ZTEST_OFF);      /* menu e abertura sem teste de Z */

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

        sfx = fopen("mass:/GRANNY/som_confirma.wav", "rb");

        if (sfx != NULL)
        {
            long sStart = 0, sSize = 0;

            if (wav_open_data(sfx, &sStart, &sSize))
            {
                if (sSize > (long)sizeof(cfmBuf))
                    sSize = (long)sizeof(cfmBuf);

                cfmBytes = (int)fread(cfmBuf, 1, (size_t)sSize, sfx);
                cfmBytes &= ~3;
            }

            fclose(sfx);
        }
    }

    if (padInit(0) == 1)
    {
        if (padPortOpen(0, 0, padBuf) != 0)
            padOpened = 1;
    }

    /* Abertura: toca o vídeo e o som enquanto as imagens do menu carregam
       em segundo plano. Se não der para usar segundo plano, carrega antes. */
    {
        int clr;

        /* Limpa as duas telas antes de carregar, senão aparece lixo de vídeo. */
        for (clr = 0; clr < 4; clr++)
        {
            clear_screen(gsGlobal, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
            gsKit_sync_flip(gsGlobal);
            gsKit_queue_exec(gsGlobal);
        }
    }

#if INTRO_BG_LOAD
    if (!start_loader(gsGlobal, sd.language))
#endif
    {
        load_assets(gsGlobal);
        loaderDone = 1;
    }

    oldButtons = play_intro(gsGlobal, audioReady, padOpened);

    /* Se a abertura acabou antes do carregamento, espera terminar. */
    while (!loaderDone)
    {
        clear_screen(gsGlobal, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
        gsKit_sync_flip(gsGlobal);
        gsKit_queue_exec(gsGlobal);
    }

    fadeLevel = 0;                        /* o menu aparece aos poucos */

    while (1)
    {
        if (fadeLevel < 256)
        {
            fadeLevel += 6;
            if (fadeLevel > 256)
                fadeLevel = 256;
        }

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

                        gPadLX = buttons.ljoy_h;  gPadLY = buttons.ljoy_v;
                        gPadRX = buttons.rjoy_h;  gPadRY = buttons.rjoy_v;
                    }
                    else
                    {
                        gPadLX = gPadLY = gPadRX = gPadRY = 128;
                    }

                    gPadHeld = currentButtons;

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
                            if (selected == 0)          /* JOGAR */
                            {
                                screen = SCREEN_PLAY;
                                playSel = PI_DIFF;
                                playPopup = 0;
                                sfx_trigger();
                            }
                            else if (selected == 1)     /* OPÇÕES */
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
                                sd.brightness += d;
                                if (sd.brightness < 0)  sd.brightness = 0;
                                if (sd.brightness > 10) sd.brightness = 10;
                            }
                            else if (optSel == OPT_LANG)
                            {
                                sd.language = (sd.language + d + LANGS) % LANGS;
                                sfx_trigger();
                            }
                            else if (optSel == OPT_MUSIC)
                            {
                                sd.musicVol += d;
                                if (sd.musicVol < 0)  sd.musicVol = 0;
                                if (sd.musicVol > 10) sd.musicVol = 10;
                                set_volumes(sd.musicVol, sd.masterVol);
                            }
                            else if (optSel == OPT_MASTER)
                            {
                                sd.masterVol += d;
                                if (sd.masterVol < 0)  sd.masterVol = 0;
                                if (sd.masterVol > 10) sd.masterVol = 10;
                                set_volumes(sd.musicVol, sd.masterVol);
                                sfx_trigger();   /* prévia do volume */
                            }
                        }

                        if ((pressedButtons & PAD_CROSS) && optSel == OPT_CREDITS)
                        {
                            screen = SCREEN_CREDITS;
                        }
                        else if ((pressedButtons & PAD_CROSS) && optSel == OPT_LANG)
                        {
                            sd.language = (sd.language + 1) % LANGS;
                            cfgDirty = 1;
                        }

                        if (pressedButtons & PAD_CROSS)
                            sfx_confirm();      /* X em qualquer linha das opções */

                        if (pressedButtons & PAD_CIRCLE)
                        {
                            if (cfgDirty)
                            {
                                config_save(&sd);
                                cfgDirty = 0;
                            }

                            screen = SCREEN_MENU;
                            sfx_trigger();
                        }
                    }
                    else if (screen == SCREEN_PLAY)
                    {
                        if (playPopup)
                        {
                            /* Aviso aberto: X ou Círculo fecham. */
                            if (pressedButtons & (PAD_CIRCLE | PAD_CROSS))
                            {
                                playPopup = 0;
                                sfx_trigger();
                            }
                        }
                        else
                        {
                            int to = -1;

                            if (goUp)         to = playNav[playSel][0];
                            else if (goDown)  to = playNav[playSel][1];
                            else if (goLeft)  to = playNav[playSel][2];
                            else if (goRight) to = playNav[playSel][3];

                            if (to == -2)               /* seletor de dificuldade */
                            {
                                int nd = sd.difficulty + (goRight ? 1 : -1);

                                if (nd >= 0 && nd < DIFFS)
                                {
                                    sd.difficulty = nd;
                                    sfx_trigger();
                                }
                            }
                            else if (to >= 0)
                            {
                                playSel = to;
                                sfx_trigger();
                            }

                            if (pressedButtons & PAD_CROSS)
                            {
                                if (playSel == PI_SAVE)
                                {
                                    int ok = config_save(&sd);

                                    playPopup = ok ? 2 : 3;
                                    if (ok)
                                        cfgDirty = 0;
                                }
                                else if (playSel == PI_BACK)
                                {
                                    screen = SCREEN_MENU;
                                }
                                else if (playSel == PI_CONT)
                                {
                                    /* Continuar: entra na cena 3D. */
                                    if (gTps <= 0.0)
                                    {
                                        set_volumes(0, sd.masterVol);
                                        gTps = measure_tick_rate(gsGlobal, black,
                                                   (gsGlobal->Height >= 512) ? 50.0 : 59.94);
                                    }
                                    set_volumes(0, sd.masterVol);   /* sem música na cena */
                                    game_start();
                                    screen = SCREEN_GAME;
                                }
                                else if (playSel != PI_DIFF)
                                {
                                    playPopup = 1;   /* skins e conquistas */
                                }

                                if (screen != SCREEN_GAME)
                                    sfx_trigger();
                            }
                            else if (pressedButtons & PAD_CIRCLE)
                            {
                                screen = SCREEN_MENU;
                                sfx_trigger();
                            }
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
                    else if (screen == SCREEN_GAME)
                    {
                        if (pressedButtons & PAD_CIRCLE)
                            gCrouchReq = 1;

                        if (pressedButtons & PAD_START)
                            gExitReq = 1;

                        if (pressedButtons & PAD_SELECT)
                            gDebug = !gDebug;

                        if (gDebug)
                        {
                            if (pressedButtons & PAD_TRIANGLE) gDbgFlush = (gDbgFlush + 1) % 3;
                            if (pressedButtons & PAD_SQUARE)   gDbgZ = (gDbgZ + 1) % 3;
                            if (pressedButtons & PAD_L2)       gDbgPvs = !gDbgPvs;
                            if (pressedButtons & PAD_R2)       gDbgCull = !gDbgCull;
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
        clear_screen(gsGlobal, black);

        if (screen == SCREEN_CREDITS)
        {
            if (assets[sd.language].credOk)
            {
                gsKit_TexManager_bind(gsGlobal, &assets[sd.language].cred);
                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &assets[sd.language].cred,
                    0.0f, 0.0f,
                    0.0f, 0.0f,
                    (float)gsGlobal->Width,
                    (float)gsGlobal->Height,
                    (float)assets[sd.language].cred.Width,
                    (float)assets[sd.language].cred.Height,
                    1,
                    tint(sd.brightness)
                );
            }
        }
        else if (screen == SCREEN_GAME)
        {
            unsigned int nowT = cpu_ticks_now();
            float dtG = (float)((double)(unsigned int)(nowT - gLastTick) / gTps);

            gLastTick = nowT;
            gRawDt = dtG;
            if (dtG > 0.05f) dtG = 0.05f;

            game_step(dtG);

            if (gExitReq)
            {
                gExitReq = 0;
                screen = SCREEN_MENU;
                set_volumes(sd.musicVol, sd.masterVol);
            }
            else
            {
                game_draw(gsGlobal, sd.brightness);
            }
        }
        else if (screen == SCREEN_PLAY)
        {
            LangAssets *la = &assets[sd.language];
            u64 col   = tint(sd.brightness);
            u64 frame = GS_SETREG_RGBAQ(0xD0, 0x20, 0x20, 0x80, 0);
            u64 boxOn = GS_SETREG_RGBAQ(0xB4, 0x12, 0x12, 0x80, 0);
            float sx = (float)gsGlobal->Width  / 640.0f;
            float sy = (float)gsGlobal->Height / 448.0f;
            float fx1 = playRect[playSel][0] * sx;
            float fy1 = playRect[playSel][1] * sy;
            float fx2 = playRect[playSel][2] * sx;
            float fy2 = playRect[playSel][3] * sy;
            float t = 2.0f * sy;
            int i;

            if (la->playOk)
            {
                gsKit_TexManager_bind(gsGlobal, &la->play);
                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &la->play,
                    0.0f, 0.0f,
                    0.0f, 0.0f,
                    (float)gsGlobal->Width,
                    (float)gsGlobal->Height,
                    (float)la->play.Width,
                    (float)la->play.Height,
                    1,
                    col
                );
            }

            if (la->atlasOk)
            {
                int dd = sd.difficulty;

                /* nome da dificuldade e painel com os detalhes */
                draw_sub(gsGlobal, &la->atlas, 26.0f, 188.0f, 128.0f, 28.0f,
                         (float)(dd * 128), 392.0f, col, 2);
                draw_sub(gsGlobal, &la->atlas, 176.0f, 172.0f, 320.0f, 130.0f,
                         (float)((dd % 2) * 320), (float)((dd / 2) * 130),
                         col, 2);
            }

            /* Caixas marcadas: definidas pela dificuldade (travadas). */
            for (i = 0; i < 4; i++)
            {
                if (diffPreset[sd.difficulty][i])
                {
                    float cx = (i % 2 == 0) ? 383.0f : 467.0f;
                    float cy = (i / 2 == 0) ? 64.0f : 124.0f;

                    gsKit_prim_sprite(
                        gsGlobal,
                        (cx + 3.0f) * sx, (cy + 3.0f) * sy,
                        (cx + 15.0f) * sx, (cy + 15.0f) * sy,
                        2, boxOn);
                }
            }

            /* Moldura vermelha no item selecionado. */
            gsKit_prim_sprite(gsGlobal, fx1, fy1, fx2, fy1 + t, 3, frame);
            gsKit_prim_sprite(gsGlobal, fx1, fy2 - t, fx2, fy2, 3, frame);
            gsKit_prim_sprite(gsGlobal, fx1, fy1, fx1 + t, fy2, 3, frame);
            gsKit_prim_sprite(gsGlobal, fx2 - t, fy1, fx2, fy2, 3, frame);

            /* Avisos: em desenvolvimento / salvo / falha ao salvar. */
            if (playPopup == 1 && la->devOk)
            {
                float pw = (float)la->dev.Width;
                float ph = (float)la->dev.Height;

                draw_sub(gsGlobal, &la->dev, (640.0f - pw) / 2.0f,
                         (448.0f - ph) / 2.0f, pw, ph, 0.0f, 0.0f, col, 4);
            }
            else if ((playPopup == 2 || playPopup == 3) && la->atlasOk)
            {
                draw_sub(gsGlobal, &la->atlas, 160.0f, 179.0f, 320.0f, 90.0f,
                         (playPopup == 2) ? 0.0f : 320.0f, 430.0f, col, 4);
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

            if (assets[sd.language].multiOk)
            {
                gsKit_TexManager_bind(gsGlobal, &assets[sd.language].multi);
                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &assets[sd.language].multi,
                    0.0f, 0.0f,
                    0.0f, 0.0f,
                    (float)gsGlobal->Width,
                    (float)gsGlobal->Height,
                    (float)assets[sd.language].multi.Width,
                    (float)assets[sd.language].multi.Height,
                    1,
                    tint(sd.brightness)
                );
            }

            /* Moldura vermelha na opção selecionada. */
            gsKit_prim_sprite(gsGlobal, fx1, fy1, fx2, fy1 + t, 3, frame);
            gsKit_prim_sprite(gsGlobal, fx1, fy2 - t, fx2, fy2, 3, frame);
            gsKit_prim_sprite(gsGlobal, fx1, fy1, fx1 + t, fy2, 3, frame);
            gsKit_prim_sprite(gsGlobal, fx2 - t, fy1, fx2, fy2, 3, frame);

            /* Aviso "em desenvolvimento" no centro da tela. */
            if (devPopup && assets[sd.language].devOk)
            {
                GSTEXTURE *dv = &assets[sd.language].dev;
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
                    tint(sd.brightness)
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
            const int barVal[3] = {sd.brightness, sd.musicVol, sd.masterVol};
            float fy1 = (optCenterY[optSel] - OPT_ROW_HALF) * sy;
            float fy2 = (optCenterY[optSel] + OPT_ROW_HALF) * sy;
            float fx1 = OPT_ROW_X1 * sx;
            float fx2 = OPT_ROW_X2 * sx;
            float t = 2.0f * sy;
            int r, i;

            if (assets[sd.language].optOk)
            {
                gsKit_TexManager_bind(gsGlobal, &assets[sd.language].opt);
                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &assets[sd.language].opt,
                    0.0f, 0.0f,
                    0.0f, 0.0f,
                    (float)gsGlobal->Width,
                    (float)gsGlobal->Height,
                    (float)assets[sd.language].opt.Width,
                    (float)assets[sd.language].opt.Height,
                    1,
                    tint(sd.brightness)
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
        else if (assets[sd.language].menuOk)
        {
            /* Fatores de escala: as posições foram medidas em 640x448,
               mas o modo de vídeo pode ser PAL (640x512). */
            float sx = (float)gsGlobal->Width  / 640.0f;
            float sy = (float)gsGlobal->Height / 448.0f;

            gsKit_TexManager_bind(gsGlobal, &assets[sd.language].menu);
            gsKit_prim_sprite_texture(
                gsGlobal,
                &assets[sd.language].menu,
                0.0f, 0.0f,
                0.0f, 0.0f,
                (float)gsGlobal->Width,
                (float)gsGlobal->Height,
                (float)assets[sd.language].menu.Width,
                (float)assets[sd.language].menu.Height,
                1,
                tint(sd.brightness)
            );

            /* Destaque opaco só sobre o botão selecionado. */
            if (assets[sd.language].hlOk[selected])
            {
                float x1 = buttonPos[selected].x * sx;
                float y1 = buttonPos[selected].y * sy;
                float x2 = x1 + (float)assets[sd.language].hl[selected].Width  * sx;
                float y2 = y1 + (float)assets[sd.language].hl[selected].Height * sy;

                gsKit_TexManager_bind(gsGlobal, &assets[sd.language].hl[selected]);
                gsKit_prim_sprite_texture(
                    gsGlobal,
                    &assets[sd.language].hl[selected],
                    x1, y1,
                    0.0f, 0.0f,
                    x2, y2,
                    (float)assets[sd.language].hl[selected].Width,
                    (float)assets[sd.language].hl[selected].Height,
                    2,
                    tint(sd.brightness)
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
