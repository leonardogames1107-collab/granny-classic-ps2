#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <libpad.h>
#include <stdio.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <audsrv.h>

static unsigned char padBuf[256] __attribute__((aligned(64)));

int main(int argc, char *argv[])
{
    GSGLOBAL *gsGlobal;
    GSFONTM *font;
    GSTEXTURE menus[3] = {{0}};
    GSTEXTURE fallback = {0};

    int loaded[3] = {0, 0, 0};
    int fallbackLoaded = 0;
    int selected = 0;
    int padOpened = 0;
    int state;
    int audioReady = 0;
    struct audsrv_fmt_t audioFormat;

    const char *paths[3] = {
        "mass:/GRANNY/menu_jogar.png",
        "mass:/GRANNY/menu_opcoes.png",
        "mass:/GRANNY/menu_sair.png"
    };

    struct padButtonStatus buttons;
    unsigned short oldButtons = 0;
    int oldAnalogDirection = 0;
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

    for (int i = 0; i < 3; i++)
    {
        menus[i].Delayed = GS_SETTING_ON;

        if (gsKit_texture_png(gsGlobal, &menus[i], paths[i]) == 0)
            loaded[i] = 1;
    }

    fallback.Delayed = GS_SETTING_ON;

    if (gsKit_texture_png(
        gsGlobal, &fallback, "mass:/GRANNY/menu_ps2.png"
    ) == 0)
        fallbackLoaded = 1;
    
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
                        selected = (selected + 2) % 3;
                    if (buttons.ljoy_v < 80)
{
    if (oldAnalogDirection != -1)
        selected = (selected + 2) % 3;

    oldAnalogDirection = -1;
}
else if (buttons.ljoy_v > 175)
{
    if (oldAnalogDirection != 1)
        selected = (selected + 1) % 3;

    oldAnalogDirection = 1;
}
else
{
    oldAnalogDirection = 0;
}

                    if (pressedButtons & PAD_DOWN)
                        selected = (selected + 1) % 3;

                    if ((pressedButtons & PAD_CROSS) && selected == 2)
                    {
                        padPortClose(0, 0);
                        padEnd();
                        return 0;
                    }
                }
            }
        }

        gsKit_clear(gsGlobal, black);

        GSTEXTURE *currentMenu = loaded[selected]
            ? &menus[selected]
            : &fallback;

        if (loaded[selected] || fallbackLoaded)
        {
            gsKit_TexManager_bind(gsGlobal, currentMenu);

            gsKit_prim_sprite_texture(
                gsGlobal,
                currentMenu,
                0.0f, 0.0f,
                0.0f, 0.0f,
                gsGlobal->Width,
                gsGlobal->Height,
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
