// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifdef __ANDROID__
#include <android/log.h>
#define ZTBT_TAG "ztbt"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, ZTBT_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, ZTBT_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ZTBT_TAG, __VA_ARGS__)
#else
#include <stdio.h>
#define ZTBT_LOG(level, ...) (fprintf(stderr, "ztbt " level " " __VA_ARGS__), fputc('\n', stderr))
#define LOGI(...) ZTBT_LOG("I", __VA_ARGS__)
#define LOGW(...) ZTBT_LOG("W", __VA_ARGS__)
#define LOGE(...) ZTBT_LOG("E", __VA_ARGS__)
#endif
