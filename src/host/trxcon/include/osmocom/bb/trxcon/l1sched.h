#pragma once

#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#include <osmocom/core/bits.h>
#include <osmocom/core/utils.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/gsm/gsm0502.h>
#include <osmocom/core/linuxlist.h>
#include <osmocom/core/timer.h>

#define GSM_BURST_LEN		148
#define GSM_BURST_PL_LEN	116

#define GPRS_BURST_LEN		GSM_BURST_LEN
#define EDGE_BURST_LEN		444

#define GPRS_L2_MAX_LEN		54
#define EDGE_L2_MAX_LEN		155

#define L1SCHED_CH_LID_DEDIC	0x00
#define L1SCHED_CH_LID_SACCH	0x40

/* Osmocom-specific extension for PTCCH (see 3GPP TS 45.002, section 3.3.4.2).
 * Shall be used to distinguish PTCCH and PDTCH channels on a PDCH time-slot. */
#define L1SCHED_CH_LID_PTCCH	0x80

/* Is a channel related to PDCH (GPRS) */
#define L1SCHED_CH_FLAG_PDCH	(1 << 0)
/* Should a channel be activated automatically */
#define L1SCHED_CH_FLAG_AUTO	(1 << 1)
/* Is continuous burst transmission assumed */
#define L1SCHED_CH_FLAG_CBTX	(1 << 2)

#define MAX_A5_KEY_LEN		(128 / 8)
#define TRX_TS_COUNT		8

/* Forward declaration to avoid mutual include */
struct l1sched_lchan_state;
struct l1sched_meas_set;
struct l1sched_state;
struct trx_instance;
struct l1sched_ts;

enum l1sched_clck_state {
	L1SCHED_CLCK_ST_WAIT,
	L1SCHED_CLCK_ST_OK,
};

enum l1sched_burst_type {
	L1SCHED_BURST_GMSK,
	L1SCHED_BURST_8PSK,
};

/**
 * These types define the different channels on a multiframe.
 * Each channel has queues and can be activated individually.
 */
enum l1sched_lchan_type {
	L1SCHED_IDLE = 0,
	L1SCHED_FCCH,
	L1SCHED_SCH,
	L1SCHED_BCCH,
	L1SCHED_RACH,
	L1SCHED_CCCH,
	L1SCHED_TCHF,
	L1SCHED_TCHH_0,
	L1SCHED_TCHH_1,
	L1SCHED_SDCCH4_0,
	L1SCHED_SDCCH4_1,
	L1SCHED_SDCCH4_2,
	L1SCHED_SDCCH4_3,
	L1SCHED_SDCCH8_0,
	L1SCHED_SDCCH8_1,
	L1SCHED_SDCCH8_2,
	L1SCHED_SDCCH8_3,
	L1SCHED_SDCCH8_4,
	L1SCHED_SDCCH8_5,
	L1SCHED_SDCCH8_6,
	L1SCHED_SDCCH8_7,
	L1SCHED_SACCHTF,
	L1SCHED_SACCHTH_0,
	L1SCHED_SACCHTH_1,
	L1SCHED_SACCH4_0,
	L1SCHED_SACCH4_1,
	L1SCHED_SACCH4_2,
	L1SCHED_SACCH4_3,
	L1SCHED_SACCH8_0,
	L1SCHED_SACCH8_1,
	L1SCHED_SACCH8_2,
	L1SCHED_SACCH8_3,
	L1SCHED_SACCH8_4,
	L1SCHED_SACCH8_5,
	L1SCHED_SACCH8_6,
	L1SCHED_SACCH8_7,
	L1SCHED_PDTCH,
	L1SCHED_PTCCH,
	L1SCHED_SDCCH4_CBCH,
	L1SCHED_SDCCH8_CBCH,
	_L1SCHED_CHAN_MAX
};

/* Represents a burst to be transmitted */
struct l1sched_burst_req {
	uint32_t fn;
	uint8_t tn;
	uint8_t pwr;

	/* Internally used by the scheduler */
	uint8_t bid;

	ubit_t burst[EDGE_BURST_LEN];
	size_t burst_len;
};

typedef int l1sched_lchan_rx_func(struct trx_instance *trx,
				  struct l1sched_ts *ts, struct l1sched_lchan_state *lchan,
				  uint32_t fn, uint8_t bid, const sbit_t *bits,
				  const struct l1sched_meas_set *meas);

typedef int l1sched_lchan_tx_func(struct trx_instance *trx, struct l1sched_ts *ts,
				  struct l1sched_lchan_state *lchan,
				  struct l1sched_burst_req *br);

struct l1sched_lchan_desc {
	/*! Human-readable name */
	const char *name;
	/*! Human-readable description */
	const char *desc;

	/*! Channel Number (like in RSL) */
	uint8_t chan_nr;
	/*! Link ID (like in RSL) */
	uint8_t link_id;
	/*! Sub-slot number (for SDCCH and TCH/H) */
	uint8_t ss_nr;
	/*! GSMTAP channel type (see GSMTAP_CHANNEL_*) */
	uint8_t gsmtap_chan_type;

	/*! How much memory do we need to store bursts */
	size_t burst_buf_size;
	/*! Channel specific flags */
	uint8_t flags;

	/*! Function to call when burst received from PHY */
	l1sched_lchan_rx_func *rx_fn;
	/*! Function to call when data received from L2 */
	l1sched_lchan_tx_func *tx_fn;
};

struct l1sched_tdma_frame {
	/*! Downlink channel (slot) type */
	enum l1sched_lchan_type dl_chan;
	/*! Downlink block ID */
	uint8_t dl_bid;
	/*! Uplink channel (slot) type */
	enum l1sched_lchan_type ul_chan;
	/*! Uplink block ID */
	uint8_t ul_bid;
};

struct l1sched_tdma_multiframe {
	/*! Channel combination */
	enum gsm_phys_chan_config chan_config;
	/*! Human-readable name */
	const char *name;
	/*! Repeats how many frames */
	uint8_t period;
	/*! Applies to which timeslots */
	uint8_t slotmask;
	/*! Contains which lchans */
	uint64_t lchan_mask;
	/*! Pointer to scheduling structure */
	const struct l1sched_tdma_frame *frames;
};

struct l1sched_meas_set {
	/*! TDMA frame number of the first burst this set belongs to */
	uint32_t fn;
	/*! ToA256 (Timing of Arrival, 1/256 of a symbol) */
	int16_t toa256;
	/*! RSSI (Received Signal Strength Indication) */
	int8_t rssi;
};

/* Simple ring buffer (up to 8 unique measurements) */
struct l1sched_lchan_meas_hist {
	struct l1sched_meas_set buf[8];
	struct l1sched_meas_set *head;
};

/* States each channel on a multiframe */
struct l1sched_lchan_state {
	/*! Channel type */
	enum l1sched_lchan_type type;
	/*! Channel status */
	uint8_t active;
	/*! Link to a list of channels */
	struct llist_head list;

	/*! Burst type: GMSK or 8PSK */
	enum l1sched_burst_type burst_type;
	/*! Mask of received bursts */
	uint8_t rx_burst_mask;
	/*! Mask of transmitted bursts */
	uint8_t tx_burst_mask;
	/*! Burst buffer for RX */
	sbit_t *rx_bursts;
	/*! Burst buffer for TX */
	ubit_t *tx_bursts;

	/*! A primitive being sent */
	struct l1sched_ts_prim *prim;

	/*! Mode for TCH channels (see GSM48_CMODE_*) */
	uint8_t	tch_mode;

	/*! FACCH/H on downlink */
	bool dl_ongoing_facch;
	/*! pending FACCH/H blocks on Uplink */
	uint8_t ul_facch_blocks;

	/*! Downlink measurements history */
	struct l1sched_lchan_meas_hist meas_hist;
	/*! AVG measurements of the last received block */
	struct l1sched_meas_set meas_avg;

	/*! TDMA loss detection state */
	struct {
		/*! Last processed TDMA frame number */
		uint32_t last_proc;
		/*! Number of processed TDMA frames */
		unsigned long num_proc;
		/*! Number of lost TDMA frames */
		unsigned long num_lost;
	} tdma;

	/*! SACCH state */
	struct {
		/*! Cached measurement report (last received) */
		uint8_t mr_cache[GSM_MACBLOCK_LEN];
		/*! Cache usage counter */
		uint8_t mr_cache_usage;
		/*! Was a MR transmitted last time? */
		bool mr_tx_last;
	} sacch;

	/* AMR specific */
	struct {
		/*! 4 possible codecs for AMR */
		uint8_t codec[4];
		/*! Number of possible codecs */
		uint8_t codecs;
		/*! Current uplink FT index */
		uint8_t ul_ft;
		/*! Current downlink FT index */
		uint8_t dl_ft;
		/*! Current uplink CMR index */
		uint8_t ul_cmr;
		/*! Current downlink CMR index */
		uint8_t dl_cmr;
		/*! If AMR loop is enabled */
		uint8_t amr_loop;
		/*! Number of bit error rates */
		uint8_t ber_num;
		/*! Sum of bit error rates */
		float ber_sum;
	} amr;

	/*! A5/X encryption state */
	struct {
		uint8_t key[MAX_A5_KEY_LEN];
		uint8_t key_len;
		uint8_t algo;
	} a5;

	/* TS that this lchan belongs to */
	struct l1sched_ts *ts;
};

struct l1sched_ts {
	/*! Timeslot index within a frame (0..7) */
	uint8_t index;

	/*! Pointer to multiframe layout */
	const struct l1sched_tdma_multiframe *mf_layout;
	/*! Channel states for logical channels */
	struct llist_head lchans;
	/*! Queue primitives for TX */
	struct llist_head tx_prims;
	/* backpointer to its TRX */
	struct trx_instance *trx;
};

/* Represents one TX primitive in the queue of l1sched_ts */
struct l1sched_ts_prim {
	/*! Link to queue of TS */
	struct llist_head list;
	/*! Logical channel type */
	enum l1sched_lchan_type chan;
	/*! Payload length */
	size_t payload_len;
	/*! Payload */
	uint8_t payload[0];
};

/*! One scheduler instance */
struct l1sched_state {
	/*! Clock state */
	enum l1sched_clck_state state;
	/*! Local clock source */
	struct timespec clock;
	/*! Count of processed frames */
	uint32_t fn_counter_proc;
	/*! Local frame counter advance */
	uint32_t fn_counter_advance;
	/*! Count of lost frames */
	uint32_t fn_counter_lost;
	/*! Frame callback timer */
	struct osmo_timer_list clock_timer;
	/*! Frame callback */
	void (*clock_cb)(struct l1sched_state *sched);
	/*! Private data (e.g. pointer to trx instance) */
	void *data;
};

extern const struct l1sched_lchan_desc l1sched_lchan_desc[_L1SCHED_CHAN_MAX];
const struct l1sched_tdma_multiframe *l1sched_mframe_layout(
	enum gsm_phys_chan_config config, int tn);

/* Scheduler management functions */
int l1sched_init(struct trx_instance *trx, uint32_t fn_advance);
int l1sched_reset(struct trx_instance *trx, bool reset_clock);
int l1sched_shutdown(struct trx_instance *trx);

/* Timeslot management functions */
struct l1sched_ts *l1sched_add_ts(struct trx_instance *trx, int tn);
void l1sched_del_ts(struct trx_instance *trx, int tn);
int l1sched_reset_ts(struct trx_instance *trx, int tn);
int l1sched_configure_ts(struct trx_instance *trx, int tn,
	enum gsm_phys_chan_config config);
int l1sched_start_ciphering(struct l1sched_ts *ts, uint8_t algo,
	uint8_t *key, uint8_t key_len);

/* Logical channel management functions */
enum gsm_phys_chan_config l1sched_chan_nr2pchan_config(uint8_t chan_nr);
enum l1sched_lchan_type l1sched_chan_nr2lchan_type(uint8_t chan_nr,
	uint8_t link_id);

void l1sched_deactivate_all_lchans(struct l1sched_ts *ts);
int l1sched_set_lchans(struct l1sched_ts *ts, uint8_t chan_nr, int active, uint8_t tch_mode);
int l1sched_activate_lchan(struct l1sched_ts *ts, enum l1sched_lchan_type chan);
int l1sched_deactivate_lchan(struct l1sched_ts *ts, enum l1sched_lchan_type chan);
struct l1sched_lchan_state *l1sched_find_lchan(struct l1sched_ts *ts,
	enum l1sched_lchan_type chan);

/* Primitive management functions */
int l1sched_prim_alloc(void *ctx, struct l1sched_ts_prim **prim,
	size_t pl_len, uint8_t chan_nr, uint8_t link_id);
int l1sched_prim_push(struct trx_instance *trx,
	struct l1sched_ts_prim *prim, uint8_t chan_nr);

#define L1SCHED_TCH_MODE_IS_SPEECH(mode)   \
	  (mode == GSM48_CMODE_SPEECH_V1   \
	|| mode == GSM48_CMODE_SPEECH_EFR  \
	|| mode == GSM48_CMODE_SPEECH_AMR)

#define L1SCHED_TCH_MODE_IS_DATA(mode)    \
	  (mode == GSM48_CMODE_DATA_14k5  \
	|| mode == GSM48_CMODE_DATA_12k0  \
	|| mode == GSM48_CMODE_DATA_6k0   \
	|| mode == GSM48_CMODE_DATA_3k6)

#define L1SCHED_CHAN_IS_TCH(chan) \
	(chan == L1SCHED_TCHF || chan == L1SCHED_TCHH_0 || chan == L1SCHED_TCHH_1)

#define L1SCHED_CHAN_IS_SACCH(chan) \
	(l1sched_lchan_desc[chan].link_id & L1SCHED_CH_LID_SACCH)

/* FIXME: we need a better way to identify / distinguish primitives */
#define L1SCHED_PRIM_IS_RACH11(prim) \
	(prim->payload_len == sizeof(struct l1ctl_ext_rach_req))

#define L1SCHED_PRIM_IS_RACH8(prim) \
	(prim->payload_len == sizeof(struct l1ctl_rach_req))

#define L1SCHED_PRIM_IS_RACH(prim) \
	(L1SCHED_PRIM_IS_RACH8(prim) || L1SCHED_PRIM_IS_RACH11(prim))

#define L1SCHED_PRIM_IS_TCH(prim) \
	(L1SCHED_CHAN_IS_TCH(prim->chan) && prim->payload_len != GSM_MACBLOCK_LEN)

#define L1SCHED_PRIM_IS_FACCH(prim) \
	(L1SCHED_CHAN_IS_TCH(prim->chan) && prim->payload_len == GSM_MACBLOCK_LEN)

struct l1sched_ts_prim *l1sched_prim_dequeue(struct llist_head *queue,
	uint32_t fn, struct l1sched_lchan_state *lchan);
int l1sched_prim_dummy(struct l1sched_lchan_state *lchan);
void l1sched_prim_drop(struct l1sched_lchan_state *lchan);
void l1sched_prim_flush_queue(struct llist_head *list);

int l1sched_handle_rx_burst(struct trx_instance *trx, uint8_t tn,
	uint32_t fn, sbit_t *bits, uint16_t nbits,
	const struct l1sched_meas_set *meas);

/* Shared declarations for lchan handlers */
extern const uint8_t l1sched_nb_training_bits[8][26];

const char *l1sched_burst_mask2str(const uint8_t *mask, int bits);
size_t l1sched_bad_frame_ind(uint8_t *l2, struct l1sched_lchan_state *lchan);
int l1sched_send_dt_ind(struct trx_instance *trx, struct l1sched_ts *ts,
	struct l1sched_lchan_state *lchan, uint8_t *l2, size_t l2_len,
	int bit_error_count, bool dec_failed, bool traffic);
int l1sched_send_dt_conf(struct trx_instance *trx, struct l1sched_ts *ts,
	struct l1sched_lchan_state *lchan, uint32_t fn, bool traffic);
int l1sched_gsmtap_send(enum l1sched_lchan_type lchan_type, uint32_t fn, uint8_t tn,
		      uint16_t band_arfcn, int8_t signal_dbm, uint8_t snr,
		      const uint8_t *data, size_t data_len);

/* Interleaved TCH/H block TDMA frame mapping */
uint32_t l1sched_tchh_block_dl_first_fn(enum l1sched_lchan_type chan,
	uint32_t last_fn, bool facch);
bool l1sched_tchh_block_map_fn(enum l1sched_lchan_type chan,
	uint32_t fn, bool ul, bool facch, bool start);

#define l1sched_tchh_traffic_start(chan, fn, ul) \
	l1sched_tchh_block_map_fn(chan, fn, ul, 0, 1)
#define l1sched_tchh_traffic_end(chan, fn, ul) \
	l1sched_tchh_block_map_fn(chan, fn, ul, 0, 0)

#define l1sched_tchh_facch_start(chan, fn, ul) \
	l1sched_tchh_block_map_fn(chan, fn, ul, 1, 1)
#define l1sched_tchh_facch_end(chan, fn, ul) \
	l1sched_tchh_block_map_fn(chan, fn, ul, 1, 0)

/* Measurement history */
void l1sched_lchan_meas_push(struct l1sched_lchan_state *lchan, const struct l1sched_meas_set *meas);
void l1sched_lchan_meas_avg(struct l1sched_lchan_state *lchan, unsigned int n);

int l1sched_clck_handle(struct l1sched_state *sched, uint32_t fn);
void l1sched_clck_reset(struct l1sched_state *sched);