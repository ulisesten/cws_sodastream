/*
 * hashgen — generador de la tabla hash para cfg_getenv.
 *
 * Corre LA MISMA función (hash_string de core/hash_table.c) que usa el
 * runtime, así que los valores impresos son exactamente los que el
 * switch de configuration.c debe contener.
 *
 * Uso:   ./build/hashgen KEY1 KEY2 ...
 * Salida: líneas `case 0x<hex>: /* KEY * /` listas para pegar.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "hash_table.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "uso: %s KEY1 [KEY2 ...]\n"
                "imprime los case <hash> para el switch de cfg_getenv\n",
                argv[0]);
        return 1;
    }

    /* Detección de colisiones entre las claves de entrada. */
    for (int i = 1; i < argc; i++) {
        uint32_t hi = hash_string(argv[i]);
        for (int j = i + 1; j < argc; j++) {
            if (hash_string(argv[j]) == hi) {
                fprintf(stderr, "ADVERTENCIA: colision de hash entre '%s' y '%s' (0x%08x)\n",
                        argv[i], argv[j], hi);
            }
        }
    }

    for (int i = 1; i < argc; i++) {
        printf("        case 0x%08xu: /* %s */\n            return env_get(app, \"%s\");\n",
               hash_string(argv[i]), argv[i], argv[i]);
    }
    return 0;
}
