#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>

int main(int argc, char *argv[])
{
    GSGLOBAL *gsGlobal;
    GSFONTM *font;

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

    while (1)
    {
        gsKit_clear(gsGlobal, black);

        /* Fundo */
        gsKit_prim_sprite(
            gsGlobal, 0, 0,
            gsGlobal->Width, gsGlobal->Height,
            1, dark
        );

        /* Faixa superior */
        gsKit_prim_sprite(
            gsGlobal, 0, 0,
            gsGlobal->Width, 8,
            2, red
        );

        /* Painel central */
        gsKit_prim_sprite(
            gsGlobal, 150, 35,
            490, 400,
            3, black
        );

        /* Titulo */
        gsKit_fontm_print_scaled(
            gsGlobal, font,
            220, 85, 4, 1.5f,
            white, "GRANNY"
        );

        gsKit_fontm_print_scaled(
            gsGlobal, font,
            225, 125, 4, 0.7f,
            gray, "CLASSIC"
        );

        /* Opcoes */
        gsKit_prim_sprite(
            gsGlobal, 205, 210,
            435, 250,
            4, red
        );

        gsKit_fontm_print_scaled(
            gsGlobal, font,
            250, 222, 5, 0.8f,
            white, "JOGAR"
        );

        gsKit_fontm_print_scaled(
            gsGlobal, font,
            245, 285, 5, 0.8f,
            gray, "OPCOES"
        );

        gsKit_fontm_print_scaled(
            gsGlobal, font,
            265, 340, 5, 0.8f,
            gray, "SAIR"
        );

        gsKit_fontm_print_scaled(
            gsGlobal, font,
            190, 420, 5, 0.5f,
            gray, "PROJETO FAN-MADE"
        );

        gsKit_queue_exec(gsGlobal);
        gsKit_sync_flip(gsGlobal);
    }

    return 0;
}
