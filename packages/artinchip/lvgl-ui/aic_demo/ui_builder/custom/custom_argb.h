#ifndef CUSTOM_ARGB_H
#define CUSTOM_ARGB_H

#ifdef __cplusplus
extern "C" {
#endif

/* 15-LED WS2812B on J7. Paused: custom_argb_init() does not drive PD.8. */
void custom_argb_init(void);

#ifdef __cplusplus
}
#endif

#endif
