SET ANSI_NULLS ON
GO
SET QUOTED_IDENTIFIER ON
GO
-- ===================================================
-- Autor:     cws_sodastream
-- Fecha:     2026-11-15
-- Descripción: Consulta de stickers (metadatos y check)
-- ===================================================
CREATE OR ALTER PROCEDURE [dbo].[procChatStickersCons]
    @tipoConsulta SMALLINT,
    @sti_hash     CHAR(64),
    @usu_id       INT,
    @sti_ids      VARCHAR(MAX)
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE
        @CHAT_STICKER_GET   SMALLINT = 1,
        @CHAT_STICKER_CHECK SMALLINT = 2;

    -- Metadatos de un sticker.
    IF @tipoConsulta = @CHAT_STICKER_GET
    BEGIN
        SELECT sti_id, sti_hash, sti_mime, sti_bytes, sti_width, sti_height,
               sti_animated, sti_id_created_by, sti_created
        FROM dbo.opr_chat_stickers
        WHERE sti_hash = @sti_hash;
    END

    -- registered / contact_has para una lista de ids (CSV).
    IF @tipoConsulta = @CHAT_STICKER_CHECK
    BEGIN
        SELECT s.value AS sti_hash,
               CASE WHEN r.sti_id IS NULL THEN 0 ELSE 1 END AS registered,
               CASE WHEN i.uss_id IS NULL THEN 0 ELSE 1 END AS contact_has
        FROM STRING_SPLIT(ISNULL(@sti_ids, ''), ',') s
        LEFT JOIN dbo.opr_chat_stickers r
               ON r.sti_hash = s.value
        LEFT JOIN dbo.opr_usuarios_sticker i
               ON i.uss_id_sticker = s.value AND i.uss_id_usuario = @usu_id
        WHERE LEN(s.value) = 64
          AND s.value NOT LIKE '%[^0-9a-f]%';
    END
END
GO
