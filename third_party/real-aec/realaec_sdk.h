#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
    #ifdef REAL_AEC_EXPORTS
        #define REAL_AEC_API __declspec(dllexport)
    #else
        #define REAL_AEC_API __declspec(dllimport)
    #endif
#else
    #define REAL_AEC_API
#endif

REAL_AEC_API void* REAL_AEC_create(int frame_len, int sample_rate);
REAL_AEC_API void REAL_AEC_delete(void* ptr);
REAL_AEC_API void REAL_AEC_process(void* ptr, short* mic_buf, short* spk_buf, short* out_buf);

#ifdef __cplusplus
}
#endif