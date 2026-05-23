#pragma once

/**
 * @file n00b_chalk.h
 * @brief Umbrella header for the n00b chalk module.
 *
 * Single include for callers that want every public libchalk symbol.
 * Pull in the per-codec headers individually if you only need one.
 */

#include <n00b.h>

#include <chalk/n00b_chalk_codec.h>
#include <chalk/n00b_chalk_mark.h>

#include <chalk/n00b_chalk_elf.h>
#include <chalk/n00b_chalk_macho.h>
#include <chalk/n00b_chalk_macos_wrap.h>
#include <chalk/n00b_chalk_gguf.h>
#include <chalk/n00b_chalk_safetensors.h>
#include <chalk/n00b_chalk_zip.h>
#include <chalk/n00b_chalk_pyc.h>
#include <chalk/n00b_chalk_source.h>
#include <chalk/n00b_chalk_sidecar.h>
#include <chalk/n00b_chalk_certs.h>
#include <chalk/n00b_chalk_pe.h>
#include <chalk/n00b_chalk_resign.h>
