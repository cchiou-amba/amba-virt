/*
 * iav_qual_server.h
 *
 * The entry points of the IAV qualification suite that amba-virt-server calls in its
 * qualification build (AMBA_VIRT_IAV_QUAL). The production server does not contain them.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef IAV_QUAL_SERVER_H
#define IAV_QUAL_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * argv[1] is "--iav-qual" and the suite's options follow it. Returns 0, or an IavQualExit
 * code: usage error, or the device cannot be opened. Nothing is stopped or changed.
 */
int iav_qual_server_precheck(int argc, char **argv);

/*
 * Stop the production worker (SIGUSR1 to its supervisor) so a single client owns the IAV
 * pipeline, and start a sentinel process that resumes it if this process dies without calling
 * iav_qual_server_resume(). Returns 0 when the worker is stopped or none is running, 4 when it
 * could not be held.
 */
int iav_qual_server_hold(void);

/* Resume the production worker (SIGUSR2). Safe to call when nothing was held. */
void iav_qual_server_resume(void);

/*
 * Start the witness: a thread that reads the live canvas 0 frames through its own mapping of the
 * DSP partition and logs "witness: pts=<mono_pts> hash=<signed 64-bit FNV-1a of the Y plane>" once per
 * new frame. The key is the frame's timestamp: the driver counts sequence numbers per descriptor. The guest suite records the same pair for the frames it reads, so a gate
 * can check that a frame the guest read is the frame the DSP wrote. Waits for /dev/iav.
 */
void iav_qual_server_witness_start(void);

/* Run the suite once the pipeline is up. Returns an IavQualExit code. */
int iav_qual_server_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* IAV_QUAL_SERVER_H */
