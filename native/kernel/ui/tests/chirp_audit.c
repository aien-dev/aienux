/* chirp_audit.c -- measures the startup chirp (ui/chirp.c) against the numbers
 * published for GW150914 (Abbott et al., PRL 116, 061102): the signal sweeps
 * from 35 to 250 Hz, 8 cycles from 35 to 150 Hz in 0.2 s, source-frame masses
 * 36 and 29 solar masses, final mass 62, final spin 0.67, redshift 0.09. The
 * leading-order (Newtonian quadrupole) inspiral law and the Berti, Cardoso and
 * Will (2006) fit for the least-damped ringdown mode are the physics used.
 * Prints a plain report. Hosted only. */
#include <math.h>
#include <stdio.h>
#include "../chirp.h"

#define MSUN_S 4.925491e-6 /* G M_sun / c^3, seconds */

int main(void)
{
    const double m1 = 36, m2 = 29, z = 0.09, mf = 62, spin = 0.67;
    double mc = pow(m1 * m2, 0.6) / pow(m1 + m2, 0.2) * (1 + z); /* detector-frame chirp mass */
    double T = mc * MSUN_S;
    printf("GW150914, detector-frame chirp mass %.2f solar masses (published: about 30)\n", mc);
    /* leading order: time to merger tau(f) = 5/256 (pi f)^(-8/3) T^(-5/3) */
    double f0 = 35, f1 = 150;
    double tau0 = 5.0 / 256 * pow(M_PI * f0, -8.0 / 3) * pow(T, -5.0 / 3);
    double tau1 = 5.0 / 256 * pow(M_PI * f1, -8.0 / 3) * pow(T, -5.0 / 3);
    /* gravitational-wave cycles between f0 and f1: N = (1/(32 pi^(8/3))) T^(-5/3) (f0^(-5/3) - f1^(-5/3)) */
    double ncyc = pow(T, -5.0 / 3) / (32 * pow(M_PI, 8.0 / 3)) * (pow(f0, -5.0 / 3) - pow(f1, -5.0 / 3));
    printf("physics, leading order: 35 Hz is %.3f s before the merger, 150 Hz is %.4f s before; %.3f s and %.1f cycles between "
           "(published: about 0.2 s, about 8 cycles)\n", tau0, tau1, tau0 - tau1, ncyc);
    double mdet = mf * (1 + z) * MSUN_S;
    double fr = (1.5251 - 1.1568 * pow(1 - spin, 0.1292)) / (2 * M_PI * mdet);
    double q = 0.7 + 1.4187 * pow(1 - spin, -0.4990);
    printf("physics, ringdown of the final hole (mass %.0f, spin %.2f): %.0f Hz, quality %.2f, damping time %.1f ms "
           "(published: the signal tops out near 250 Hz)\n", mf, spin, fr, q, 1000 * q / (M_PI * fr));
    printf("\nthe chirp in the kernel (ui/chirp.c), pitch by time before the merger:\n");
    printf("  ms before merger   Hz     level (of %d)\n", CK_CHIRP_PEAK);
    uint32_t mf_frame = CK_CHIRP_RATE / 1000u * CK_NIGHT_MERGE_MS;
    static const uint32_t before[] = {600, 500, 400, 300, 200, 100, 50, 20, 10, 5, 0};
    for (unsigned i = 0; i < sizeof before / sizeof *before; i++) {
        uint32_t fr_ = mf_frame - CK_CHIRP_RATE / 1000u * before[i];
        printf("  %5u            %5u  %5u\n", before[i], ck_chirp_hz(fr_), ck_chirp_level(fr_));
    }
    /* where the pitch stops rising, and how long the sweep lasts in the kernel */
    uint32_t cap = 0;
    for (uint32_t f = mf_frame - CK_CHIRP_RATE / 1000u * CK_CHIRP_RISE_MS; f < mf_frame; f++)
        if (ck_chirp_hz(f) >= CK_CHIRP_TO_HZ - 1) {
            cap = f;
            break;
        }
    printf("the pitch reaches %u Hz %.1f ms before the merger and holds; the sweep %u to %u Hz lasts %.0f ms\n", CK_CHIRP_TO_HZ,
           (double)(mf_frame - cap) * 1000 / CK_CHIRP_RATE, CK_CHIRP_FROM_HZ, CK_CHIRP_TO_HZ,
           (double)(cap - (mf_frame - CK_CHIRP_RATE / 1000u * CK_CHIRP_RISE_MS)) * 1000 / CK_CHIRP_RATE);
    /* cycles of the sound over the sweep: integrate the pitch */
    double cyc = 0;
    for (uint32_t f = mf_frame - CK_CHIRP_RATE / 1000u * CK_CHIRP_RISE_MS; f < cap; f++)
        cyc += (double)ck_chirp_hz(f) / CK_CHIRP_RATE;
    printf("cycles in the sweep: %.0f (published signal: about 8 between 35 and 150 Hz, perhaps 10 to the peak)\n", cyc);
    /* the ring after the merger: the level falls by e each 30 ms (DECAY) */
    double l0 = ck_chirp_level(mf_frame), l1 = ck_chirp_level(mf_frame + CK_CHIRP_RATE / 1000u * 30);
    printf("ring after the merger: level falls to %.2f of itself in 30 ms (time constant %.1f ms; physics: %.1f ms), held at %u Hz "
           "(physics: %.0f Hz)\n", l1 / l0, -30.0 / log(l1 / l0), 1000 * q / (M_PI * fr), CK_CHIRP_TO_HZ, fr);
    /* the ratios if the sound were the signal sped up uniformly */
    printf("pitch ratio, sound over signal: start %.2f (%u / 35), top %.2f (%u / 250); sweep length over the law's %.3f s: %.2f\n",
           CK_CHIRP_FROM_HZ / 35.0, CK_CHIRP_FROM_HZ, CK_CHIRP_TO_HZ / 250.0, CK_CHIRP_TO_HZ, tau0,
           (double)(mf_frame - (mf_frame - CK_CHIRP_RATE / 1000u * CK_CHIRP_RISE_MS)) / CK_CHIRP_RATE / tau0);

    /* ---- the picture: the black holes in ui/night.c, in design pixels (1920 wide) ---- */
    const double E = 60, shadow = 20, a0 = 270, orbit_ms = 5200, spiral_s = 6.0; /* night.c: e_full, rs_full, a0, ORBIT_MS */
    const double bc = sqrt(27.0) / 2;                                        /* shadow radius of a Schwarzschild hole, in Rs */
    printf("\nblack-hole picture (ui/night.c): Einstein radius %.0f px, shadow radius %.0f px, ratio %.2f\n", E, shadow, E / shadow);
    printf("  a hole's shadow is sqrt(27)/2 = %.4f Rs; the point-lens Einstein angle (source far behind) is sqrt(2 Rs / D). "
           "A ratio of %.2f means the viewer is D = %.1f Rs from the hole (%.0f km for 36 solar masses)\n", bc, E / shadow,
           pow(E / shadow * bc, 2) / 2, pow(E / shadow * bc, 2) / 2 * 2 * 36 * 1.476625);
    double rs_px = shadow / bc;
    printf("  so 1 Rs is %.2f px; the wide orbit's separation %.0f px = %.1f Rs; its Einstein radius %.1f Rs\n", rs_px, a0, a0 / rs_px,
           E / rs_px);
    double M = (m1 + m2) * (1 + z) * MSUN_S;               /* detector-frame total mass, s */
    double sep_m = a0 / rs_px; /* in GM/c^2 of the pair: one hole of half the mass has Rs = GM/c^2 */
    double w = pow(sep_m, -1.5) / M;                        /* orbital angular frequency, rad/s */
    double fgw = w / M_PI;
    double tau = 5.0 / 256 * pow(M_PI * fgw, -8.0 / 3) * pow(T, -5.0 / 3);
    printf("  physics at that separation: orbital period %.2f s, gravitational-wave pitch %.2f Hz, real time to the merger %.0f s "
           "(the picture: orbit %.2f s, %.0f s to the merger)\n", 2 * M_PI / w, fgw, tau, orbit_ms / 1000, spiral_s);
    printf("  time compression of the inspiral in the picture: %.1f times; orbit slowed %.1f times at the start\n", tau / spiral_s,
           orbit_ms / 1000 / (2 * M_PI / w));
    double mf_over = mf / ((m1 + m2) / 2);
    printf("  merger: final mass over each initial mass (equal masses assumed) %.2f; the picture's shadow grows %.2f times "
           "(ends at 2.0) and its Einstein radius %.3f times (physics sqrt of the mass ratio: %.3f)\n", mf_over, 2.0, 362.0 / 256,
           sqrt(mf_over));
    printf("  the bend is eased to zero between 2.2 E and 4.4 E: the point-lens pull E^2/theta there is %.1f px at 2.2 E and %.1f px at "
           "4.4 E that the picture leaves out\n", E / 2.2, E / 4.4);
    return 0;
}
