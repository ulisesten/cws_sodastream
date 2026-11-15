SET ANSI_NULLS ON
GO
SET QUOTED_IDENTIFIER ON
GO
-- ===================================================
-- Autor:     cws_sodastream
-- Fecha:     2026-11-15
-- Descripción: Registro de stickers y sync de inventario
--              (requiere opr_chat_stickers / opr_usuarios_sticker)
-- ===================================================
CREATE OR ALTER PROCEDURE [dbo].[procChatStickersProc]
    @tipoRegistro      SMALLINT,
    @sti_hash          CHAR(64),
    @sti_mime          VARCHAR(32),
    @sti_bytes         INT,
    @sti_width         INT,
    @sti_height        INT,
    @sti_animated      BIT,
    @sti_id_created_by INT,
    @usu_id            INT,
    @sti_ids           VARCHAR(MAX)
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE
        @CHAT_STICKER_INS  SMALLINT = 1,
        @CHAT_STICKER_SYNC SMALLINT = 2;

    -- ---------------------------------------------------------------
    -- 1) Registrar sticker (idempotente por sti_hash)
    -- ---------------------------------------------------------------
    IF @tipoRegistro = @CHAT_STICKER_INS
    BEGIN
        IF @sti_hash IS NULL
           OR LEN(LTRIM(RTRIM(@sti_hash))) <> 64
           OR @sti_hash LIKE '%[^0-9a-f]%'
        BEGIN
            SELECT success = 'false', msg = 'Sticker id invalido', error = 1;
            RETURN;
        END

        IF @sti_bytes IS NULL OR @sti_bytes <= 0 OR @sti_bytes > 1048576
        BEGIN
            SELECT success = 'false', msg = 'Tamano de sticker invalido', error = 1;
            RETURN;
        END

        IF @sti_id_created_by IS NULL
        BEGIN
            SELECT success = 'false', msg = 'Creador requerido', error = 1;
            RETURN;
        END

        DECLARE @exists BIT = 0;

        IF EXISTS (SELECT 1 FROM dbo.opr_chat_stickers WHERE sti_hash = @sti_hash)
            SET @exists = 1;

        IF @exists = 0
        BEGIN
            BEGIN TRY
                INSERT INTO dbo.opr_chat_stickers (
                    sti_hash, sti_mime, sti_bytes, sti_width, sti_height,
                    sti_animated, sti_id_created_by
                ) VALUES (
                    @sti_hash, ISNULL(@sti_mime, 'image/webp'), @sti_bytes,
                    @sti_width, @sti_height, ISNULL(@sti_animated, 0),
                    @sti_id_created_by
                );
            END TRY
            BEGIN CATCH
                -- Carrera: otro cliente insertó el mismo id.
                IF EXISTS (SELECT 1 FROM dbo.opr_chat_stickers WHERE sti_hash = @sti_hash)
                    SET @exists = 1;
                ELSE
                BEGIN
                    SELECT success = 'false', msg = ERROR_MESSAGE(), error = 1;
                    RETURN;
                END
            END CATCH
        END

        SELECT success = 'true',
               msg = CASE WHEN @exists = 1 THEN 'Sticker existente'
                          ELSE 'Sticker registrado' END,
               error = 0,
               [exists] = @exists,
               sti_id = sti_id,
               sti_hash = sti_hash,
               sti_mime = sti_mime,
               sti_bytes = sti_bytes,
               sti_width = sti_width,
               sti_height = sti_height,
               sti_animated = sti_animated
        FROM dbo.opr_chat_stickers
        WHERE sti_hash = @sti_hash;
    END

    -- ---------------------------------------------------------------
    -- 2) Sincronizar inventario local (reemplazo)
    -- ---------------------------------------------------------------
    IF @tipoRegistro = @CHAT_STICKER_SYNC
    BEGIN
        IF @usu_id IS NULL
        BEGIN
            SELECT success = 'false', msg = 'usu_id requerido', error = 1;
            RETURN;
        END

        BEGIN TRY
            BEGIN TRAN;

            DELETE FROM dbo.opr_usuarios_sticker WHERE uss_id_usuario = @usu_id;

            INSERT INTO dbo.opr_usuarios_sticker (uss_id_usuario, uss_id_sticker)
            SELECT @usu_id, value
            FROM STRING_SPLIT(ISNULL(@sti_ids, ''), ',')
            WHERE LEN(value) = 64
              AND value NOT LIKE '%[^0-9a-f]%'
            GROUP BY value;

            DECLARE @synced INT = @@ROWCOUNT;

            COMMIT;

            SELECT success = 'true', msg = 'Inventario sincronizado',
                   error = 0, synced = @synced;
        END TRY
        BEGIN CATCH
            IF @@TRANCOUNT > 0 ROLLBACK;
            SELECT success = 'false', msg = ERROR_MESSAGE(), error = 1;
        END CATCH
    END
END
GO
