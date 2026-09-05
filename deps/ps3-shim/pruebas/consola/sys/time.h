#ifndef FALSA_CONSOLA_TIME_H
#define FALSA_CONSOLA_TIME_H
#include_next <sys/time.h>
#undef timercmp
#undef timeradd
#undef timersub
#endif
