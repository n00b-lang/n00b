/** @file src/chalk/init.c — module initialization placeholder.
 *
 *  Real per-codec implementations land alongside this file as the
 *  port progresses. This unit currently only provides a module
 *  identifier so the chalk subdirectory has at least one translation
 *  unit during early scaffolding. */

#include <n00b.h>
#include <chalk/n00b_chalk.h>

n00b_string_t *
n00b_chalk_module_name(void)
{
    return r"n00b_chalk";
}
