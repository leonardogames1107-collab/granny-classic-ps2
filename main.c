#include <tamtypes.h>
#include <kernel.h>
#include <debug.h>

int main(int argc, char *argv[])
{
    init_scr();

    scr_printf("GRANNY CLASSIC PS2\n");
    scr_printf("====================\n\n");

    scr_printf("Projeto iniciado!\n");
    scr_printf("PlayStation 2 Slim\n\n");

    scr_printf("Versao classica\n");
    scr_printf("Aranha do sotao: MANTIDA\n");
    scr_printf("Aranha do esgoto: REMOVIDA\n\n");

    scr_printf("Desenvolvimento em andamento...\n");

    SleepThread();

    return 0;
}
