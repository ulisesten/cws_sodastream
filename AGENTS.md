# Instrucciones para agentes — cws_sodastream

## Documentación de estado (obligatoria)

Cada vez que se documente una implementación, un requerimiento o una auditoría:

1. **Marcar el estado real**, sin ambigüedad:
   - `[IMPLEMENTADO]` — existe código/config y evidencia (archivo/commit/test).
   - `[PARCIAL]` — una parte existe; indicar exactamente qué falta.
   - `[CLIENTE]` — requerido en la app; el servidor no lo resuelve.
   - `[PENDIENTE]` — no existe todavía.
   - `[BLOQUEADO]` — depende de una decisión, credencial, acceso o tercero.
   - `[OPS]` — configuración/despliegue, no código de la app.
2. **Separar planos**: servidor (`cws_sodastream`), cliente (Android/Qt),
   fork/lib (GOST/WebRTC), BD y operación. No mezclar “implementado” de un
   plano con otro.
3. **Cada documento de feature debe empezar con un bloque `Estado`:**
   - Implementado (servidor)
   - Implementado (cliente)
   - Pendiente (servidor)
   - Requerido al cliente
   - Bloqueado / decisiones
   - Evidencia (archivo, commit, test)
4. **No declarar “implementado” sin evidencia.** Si no se probó E2E, decirlo.
5. **Si falta algo, listarlo explícitamente** con el plano y el siguiente
   paso; no dejarlo implícito.
6. **Actualizar `docs/IMPLEMENTATION_STATUS.md`** en el mismo commit que el
   cambio. Es la fuente única de verdad.
7. **Los requerimientos no son estado**: un documento de requerimiento puede
   describir lo que se desea; su estado va en el bloque `Estado`.
8. **Seguridad**: no incluir secretos, tokens, claves, SDP ni teléfonos en
   docs, logs o commits.

Usa esta plantilla al crear/actualizar documentación:

```md
## Estado

- [IMPLEMENTADO][servidor] ...
- [IMPLEMENTADO][cliente] ...
- [PARCIAL][servidor] ...; falta ...
- [PENDIENTE][cliente] ...
- [BLOQUEADO][ops] ... (depende de ...)
- Evidencia: `archivo`, commit `...`, test `...`
```

La regla aplica a `docs/`, README, comentarios de API y mensajes de commit.
