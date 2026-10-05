/*
 * ICDA port: no-op stand-ins for iwm(4) features that were removed from
 * this port (LEDs, A-MPDU reordering and Tx aggregation, the OpenBSD
 * debug dump).  Included by if_iwm.c after if_iwmvar.h.
 *
 * Copyright (c) 2026 ICDA contributors
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef ICDA_IWM_PORT_H
#define ICDA_IWM_PORT_H

/* LEDs */
#define iwm_led_enable(sc)		((void)(sc))
#define iwm_led_blink_start(sc)		((void)(sc))
#define iwm_led_blink_stop(sc)		((void)(sc))

/* A-MPDU Rx reordering and Tx aggregation */
#define iwm_clear_reorder_buffer(sc, rxba)	((void)(sc), (void)(rxba))
#define iwm_sta_tx_agg(sc, ni, tid, ssn, winsize, start) \
	((void)(sc), (void)(ni), (void)(tid), (void)(ssn), (void)(winsize), \
	 (void)(start), 0)

/* Debug dump of driver state on firmware errors */
#define iwm_dump_driver_status(sc)	((void)(sc))

/* ICDA glue entry points (wifi.c) */
struct iwm_softc;
int	iwm_icda_attach(struct iwm_softc *);
int	iwm_init(struct ifnet *);
void	iwm_start(struct ifnet *);
void	iwm_stop(struct ifnet *);
int	iwm_intr(void *);

#endif /* ICDA_IWM_PORT_H */
