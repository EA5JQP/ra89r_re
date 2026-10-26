/* The RA89R startup's entry point over the K1's `Main()`.
 *
 * Port adaptation (from the K5V3's Core/Src/main.c shape — the vendor wrapper
 * that branches into the application's `Main()` after its clock config): on
 * this board the clock setup belongs to `board/clock.c` (the validated 48 MHz
 * bring-up) which `BOARD_Init()` calls, so this is just the symbol the RA89R
 * startup_py32f403xx.s branches to.
 */
#include "driver/bk4819.h"      /* the K1 App header that declares Main() */

int main(void)
{
    Main();
    return 0;                   /* unreachable: Main() loops */
}
