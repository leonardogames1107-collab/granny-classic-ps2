#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <libpad.h>

static unsigned char padBuf[256] __attribute__((aligned(64)));

int main(int argc, char *argv[])
{
    GSGLOBAL *gsGlobal;
    GSFONTM *font;
    GSTEXTURE background = {0};
    int backgroundLoaded = 0;

    struct padButtonStatus buttons;
    unsigned short oldButtons = 0;
    unsigned short currentButtons;
    unsigned short pressedButtons;

    int selected = 0;
    int padInitialized = 0;
    int padOpened = 0;
    int state;
    const char *message = "";

    u64 black = GS_SETREG_RGBAQ(5, 5, 8, 0, 0);
    u64 dark = GS_SETREG_RGBAQ(20, 20, 25, 0, 0);
    u64 red = GS_SETREG_RGBAQ(130, 15, 20, 0, 0);
    u64 white = GS_SETREG_RGBAQ(240, 240, 230, 0, 0);
    u64 gray = GS_SETREG_RGBAQ(100, 100, 100, 0, 0);

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
    
    /* Carregar imagem de fundo */
    background.Delayed = GS_SETTING_ON;

if (gsKit_texture_png(
    gsGlobal,
    &background,
    "mass:/GRANNY/menu_ps2.png"
) == 0)
{
    backgroundLoaded = 1;
    message = "IMAGEM CARREGADA!";
}
else
{
    message = "ERRO AO CARREGAR IMAGEM";
}

    /* Inicializar controle */
    if (padInit(0) == 1)
    {
        padInitialized = 1;

        if (padPortOpen(0, 0, padBuf) != 0)
            padOpened = 1;
    }

    while (1)
    {
        /* Ler controle */
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
                        selected = (selected + 2) % 3;
                        message = "";
                    }

                    if (pressedButtons & PAD_DOWN)
                    {
                        selected = (selected + 1) % 3;
                        message = "";
                    }

                    if (pressedButtons & PAD_CROSS)
                    {
                        if (selected == 0)
                            message = "JOGO EM DESENVOLVIMENTO";

                        if (selected == 1)
                            message = "OPCOES EM DESENVOLVIMENTO";

                        if (selected == 2)
                        {
                            padPortClose(0, 0);
                            padEnd();
                            return 0;
                        }
                    }

                    if (pressedButtons & PAD_CIRCLE)
                        message = "";
                }
            }
        }

        gsKit_clear(gsGlobal, black);
     if (backgroundLoaded)
{
    gsKit_TexManager_bind(gsGlobal, &background);

    gsKit_prim_sprite_texture(
        gsGlobal,
        &background,
        0.0f, 0.0f,
        0.0f, 0.0f,
        gsGlobal->Width,
        gsGlobal->Height,
        background.Width,
        background.Height,
        1,
        GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0)
    );
}
        /* Destaque vermelho da opção selecionada */
int highlightY = selected == 0 ? 124 :
                 selected == 1 ? 156 : 188;

int left = (32 * gsGlobal->Width) / 512;
int right = (160 * gsGlobal->Width) / 512;
int top = (highlightY * gsGlobal->Height) / 256;
int bottom = ((highlightY + 40) * gsGlobal->Height) / 256;

gsKit_prim_sprite(gsGlobal, left, top, right, top + 3, 2, red);
gsKit_prim_sprite(gsGlobal, left, bottom - 3, right, bottom, 2, red);
gsKit_prim_sprite(gsGlobal, left, top, left + 3, bottom, 2, red);
gsKit_prim_sprite(gsGlobal, right - 3, top, right, bottom, 2, red);

        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }

    return 0;
}
