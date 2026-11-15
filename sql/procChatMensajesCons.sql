SET ANSI_NULLS ON
GO
SET QUOTED_IDENTIFIER ON
GO
-- ===================================================
-- Autor:     cws_sodastream
-- Fecha:     2026-11-13
-- Descripción: Consulta de mensajes de chat 1:1
--              (requiere dbo.opr_chat_mensajes)
-- ===================================================
CREATE OR ALTER PROCEDURE [dbo].[procChatMensajesCons]
    @tipoConsulta SMALLINT,
    @usu_id       INT,
    @usu_id_otro  INT,
    @msg_before   BIGINT,
    @limit        INT
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE @CHAT_MSG_HISTORY SMALLINT = 1;

    -- Historial de la conversación (usu_id, usu_id_otro), más nuevo primero.
    IF @tipoConsulta = @CHAT_MSG_HISTORY
    BEGIN
        IF @usu_id IS NULL OR @usu_id_otro IS NULL
        BEGIN
            SELECT success = 'false', msg = 'usu_id y usu_id_otro son requeridos',
                   error = 1;
            RETURN;
        END

        DECLARE @menor INT = CASE WHEN @usu_id < @usu_id_otro
                                  THEN @usu_id ELSE @usu_id_otro END;
        DECLARE @mayor INT = CASE WHEN @usu_id > @usu_id_otro
                                  THEN @usu_id ELSE @usu_id_otro END;
        DECLARE @n INT = CASE
            WHEN @limit IS NULL OR @limit <= 0 THEN 30
            WHEN @limit > 100 THEN 100
            ELSE @limit
        END;

        SELECT TOP (@n)
            m.msg_id,
            m.msg_usu_emisor,
            m.msg_usu_receptor,
            e.usu_id_public AS emisor_public,
            m.msg_tipo,
            m.msg_texto,
            m.msg_sticker_id,
            s.sti_mime AS sticker_mime,
            s.sti_width AS sticker_width,
            s.sti_height AS sticker_height,
            s.sti_animated AS sticker_animated,
            m.msg_fecha,
            m.msg_leido,
            m.msg_fecha_lectura,
            m.msg_id_respuesta
        FROM dbo.opr_chat_mensajes m
        LEFT JOIN dbo.cat_usuarios e
               ON e.usu_id = m.msg_usu_emisor
        LEFT JOIN dbo.opr_chat_stickers s
               ON s.sti_hash = m.msg_sticker_id
        WHERE m.msg_usu_menor = @menor
          AND m.msg_usu_mayor = @mayor
          AND (@msg_before IS NULL OR m.msg_id < @msg_before)
          AND m.msg_eliminado = 0
        ORDER BY m.msg_id DESC;
    END
END
GO
