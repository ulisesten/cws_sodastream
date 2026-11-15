-- ============================================================
-- Propuesta: stickers del chat 1:1
-- Autor: cws_sodastream
-- Fecha: 2026-11-15
-- ============================================================
-- Requiere dbo.opr_chat_mensajes (ya aplicada).
-- El archivo vive en STICKERS_DIR/<sti_hash>.webp (sti_hash = Streebog-256 hex).
-- msg_tipo: 0 = texto, 1 = sticker (la API usa "text"/"sticker").
-- Convención: cada tabla tiene su PK int identity "prefijo_id".
--
-- Si aplicaste una versión anterior de estas tablas, recréalas (no hay datos):
--   DROP TABLE dbo.opr_usuarios_sticker;
--   DROP TABLE dbo.opr_chat_stickers;
-- ============================================================

IF OBJECT_ID('dbo.opr_chat_stickers', 'U') IS NULL
BEGIN
    CREATE TABLE dbo.opr_chat_stickers (
        sti_id            INT IDENTITY(1,1) NOT NULL
            CONSTRAINT PK_opr_chat_stickers PRIMARY KEY,
        -- Hash de contenido (Streebog-256 hex): id público del sticker.
        sti_hash          CHAR(64)      NOT NULL
            CONSTRAINT UQ_opr_chat_stickers_hash UNIQUE,
        sti_mime          VARCHAR(32)   NOT NULL
            CONSTRAINT DF_opr_chat_stickers_mime DEFAULT ('image/webp'),
        sti_bytes         INT           NOT NULL,
        sti_width         INT           NULL,
        sti_height        INT           NULL,
        sti_animated      BIT           NOT NULL
            CONSTRAINT DF_opr_chat_stickers_anim DEFAULT (0),
        -- Usuario que subió/registró el sticker (interno).
        sti_id_created_by INT           NOT NULL,
        sti_created       DATETIME2     NOT NULL
            CONSTRAINT DF_opr_chat_stickers_created DEFAULT (SYSUTCDATETIME())
    );
END
GO

-- Nota: si aplicaste la versión anterior (opr_usuarios_stickers), puedes
-- eliminarla: DROP TABLE dbo.opr_usuarios_stickers;
IF OBJECT_ID('dbo.opr_usuarios_sticker', 'U') IS NULL
BEGIN
    CREATE TABLE dbo.opr_usuarios_sticker (
        uss_id         INT IDENTITY(1,1) NOT NULL
            CONSTRAINT PK_opr_usuarios_sticker PRIMARY KEY,
        -- Prefijo uss: los id van justo después del prefijo + entidad.
        uss_id_usuario INT           NOT NULL,
        uss_id_sticker CHAR(64)      NOT NULL, -- hash de contenido
        uss_fecha_alta DATETIME2     NOT NULL
            CONSTRAINT DF_opr_usuarios_sticker_fecha DEFAULT (SYSUTCDATETIME()),
        CONSTRAINT UQ_opr_usuarios_sticker
            UNIQUE (uss_id_usuario, uss_id_sticker)
    );
    CREATE INDEX ix_uss_sticker ON dbo.opr_usuarios_sticker (uss_id_sticker);
END
GO

-- Columna nueva en mensajes (solo si falta).
IF COL_LENGTH('dbo.opr_chat_mensajes', 'msg_sticker_id') IS NULL
    ALTER TABLE dbo.opr_chat_mensajes ADD msg_sticker_id CHAR(64) NULL;
GO

-- El check viejo exigía texto no vacío; los stickers llevan texto ''.
IF EXISTS (SELECT 1 FROM sys.check_constraints
            WHERE name = 'CK_opr_chat_mensajes_texto'
              AND parent_object_id = OBJECT_ID('dbo.opr_chat_mensajes'))
    ALTER TABLE dbo.opr_chat_mensajes DROP CONSTRAINT CK_opr_chat_mensajes_texto;
GO

ALTER TABLE dbo.opr_chat_mensajes ADD CONSTRAINT CK_opr_chat_mensajes_texto
    CHECK (
        (msg_tipo = 0
            AND LEN(LTRIM(RTRIM(msg_texto))) > 0
            AND msg_sticker_id IS NULL)
        OR
        (msg_tipo = 1
            AND LEN(LTRIM(RTRIM(msg_sticker_id))) = 64)
    );
GO
