#ifndef SOLOADER_VIDEO_AVC_H
#define SOLOADER_VIDEO_AVC_H

#ifdef __cplusplus
extern "C" {
#endif

// Reproduce un MP4 H.264(+AAC) con sceAvcdec por hardware (ver video_avc.c).
// Bloqueante; Cross/START saltan. Devuelve 0 si el archivo no es H.264 o el
// decoder no pudo arrancar (el llamador puede caer a SceAvPlayer).
int video_play_avc(const char *path);

#ifdef __cplusplus
}
#endif

#endif
