/* Flite's library has ALSA playback built in; icda-say only writes WAV files,
 * so its ALSA calls are stubs that fail (they are never reached). */
#define STUB(name) int name(void) { return -1; }
STUB(snd_config_update_free_global) STUB(snd_pcm_close) STUB(snd_pcm_drain) STUB(snd_pcm_drop)
STUB(snd_pcm_hw_params) STUB(snd_pcm_hw_params_any) STUB(snd_pcm_hw_params_set_access)
STUB(snd_pcm_hw_params_set_channels) STUB(snd_pcm_hw_params_set_format) STUB(snd_pcm_hw_params_set_rate)
STUB(snd_pcm_open) STUB(snd_pcm_prepare) STUB(snd_pcm_resume) STUB(snd_pcm_set_params) STUB(snd_pcm_state)
STUB(snd_pcm_wait) STUB(snd_pcm_writei)
unsigned long snd_pcm_hw_params_sizeof(void) { return 64; }
const char *snd_strerror(int e) { (void)e; return "no ALSA"; }
