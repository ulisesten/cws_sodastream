#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * hash_table — función de hash de cadenas para dispatch de claves.
 *
 * Un solo requisito por diseño: pasar de cadena a entero de forma
 * determinista y barata, para que las claves de configuración se
 * despachen con un switch sobre valores generados por el mismo
 * algoritmo (scripts/hashgen.c + scripts/gen_env_hashes.sh).
 *
 * Se eligió FNV-1a de 32 bits: simple, sin dependencias, buena
 * distribución para claves cortas (nombres de env) y estable entre
 * ejecuciones — el valor generado una vez se puede pegar como case
 * constante en el código.
 */

/**
 * \brief Hash FNV-1a (32 bits) de una cadena NUL-terminada.
 *
 * \param[in] s cadena a hashear; NULL devuelve 0.
 * \return entero de 32 bits determinista (0 para NULL/cadena vacía).
 */
uint32_t hash_string(const char* s);

#ifdef __cplusplus
}
#endif

#endif /* HASH_TABLE_H */
