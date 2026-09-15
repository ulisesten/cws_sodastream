-- 0001_password_hash_nvarchar255.sql
--
-- Fix #2: los hashes de contraseña del server C usan PBKDF2-HMAC-Streebog
-- con formato auto-descriptivo (~121 chars):
--
--   $gost-pbkdf2$512$<iter>$<salt_hex>$<dk_hex>
--
-- La columna y el parámetro del SP eran NVARCHAR(64) (heredado del
-- Streebog-256 hex), lo que truncaba el hash. Se amplían a NVARCHAR(255).
--
-- Compatibilidad: el server JS de referencia sigue guardando 64 chars y
-- caben sin problema.
--
-- IMPORTANTE: hay que ampliar TAMBIÉN el parámetro @usu_contrasena de
-- procUsersProc (ALTER PROCEDURE). Aquí se deja el ALTER TABLE; regenerar el
-- procedimiento con @usu_contrasena NVARCHAR(255).

ALTER TABLE dbo.cat_usuarios
    ALTER COLUMN usu_contrasena NVARCHAR(255) NULL;

-- El parámetro equivalente en el stored procedure (aplicar sobre la
-- definición real de procUsersProc):
--
--   @usu_contrasena nvarchar(255),
