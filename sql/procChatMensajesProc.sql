SET ANSI_NULLS ON
GO
SET QUOTED_IDENTIFIER ON
GO
-- ===================================================
-- Autor:     cws_sodastream
-- Fecha:     2026-11-13
-- Descripción: Operaciones de mensajes de chat 1:1
--              (requiere dbo.opr_chat_mensajes)
-- ===================================================
CREATE OR ALTER PROCEDURE [dbo].[procChatMensajesProc]
    @tipoRegistro     SMALLINT,
    @msg_usu_emisor   INT,
    @msg_usu_receptor INT,
    @msg_texto        NVARCHAR(4000),
    @msg_id_respuesta BIGINT,
    @msg_usu_lector   INT,
    @msg_usu_otro     INT,
    @msg_hasta        BIGINT,
    @error            INT,
    @msg              VARCHAR(150),
    @error_state      INT,
    @error_sev        INT,
    @success          VARCHAR(5)
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE
        @CHAT_MSG_INS  SMALLINT = 1,
        @CHAT_MSG_READ SMALLINT = 2;

    -- ---------------------------------------------------------------
    -- 1) Insertar mensaje
    -- ---------------------------------------------------------------
    IF @tipoRegistro = @CHAT_MSG_INS
    BEGIN
        IF @msg_usu_emisor IS NULL OR @msg_usu_receptor IS NULL
        BEGIN
            SELECT success = 'false', msg = 'Emisor y receptor son requeridos',
                   error = 1;
            RETURN;
        END

        IF @msg_usu_emisor = @msg_usu_receptor
        BEGIN
            SELECT success = 'false', msg = 'No se puede enviar un mensaje a uno mismo',
                   error = 1;
            RETURN;
        END

        IF @msg_texto IS NULL OR LEN(LTRIM(RTRIM(@msg_texto))) = 0
        BEGIN
            SELECT success = 'false', msg = 'El mensaje esta vacio', error = 1;
            RETURN;
        END

        -- Ambos deben tener id público (mensajería configurada).
        IF (SELECT COUNT(*)
              FROM dbo.cat_usuarios
             WHERE usu_id IN (@msg_usu_emisor, @msg_usu_receptor)
               AND LEN(LTRIM(RTRIM(usu_id_public))) = 12) < 2
        BEGIN
            SELECT success = 'false',
                   msg = 'Emisor o receptor sin id publico (mensajeria no configurada)',
                   error = 1;
            RETURN;
        END

        IF @msg_id_respuesta IS NOT NULL AND NOT EXISTS (
            SELECT 1
            FROM dbo.opr_chat_mensajes
            WHERE msg_id = @msg_id_respuesta
              AND msg_usu_menor = CASE WHEN @msg_usu_emisor < @msg_usu_receptor
                                       THEN @msg_usu_emisor ELSE @msg_usu_receptor END
              AND msg_usu_mayor = CASE WHEN @msg_usu_emisor > @msg_usu_receptor
                                       THEN @msg_usu_emisor ELSE @msg_usu_receptor END
              AND msg_eliminado = 0
        )
        BEGIN
            SELECT success = 'false', msg = 'Mensaje a responder invalido',
                   error = 1;
            RETURN;
        END

        BEGIN TRY
            DECLARE @fecha DATETIME = GETDATE();

            INSERT INTO dbo.opr_chat_mensajes (
                msg_usu_emisor,
                msg_usu_receptor,
                msg_tipo,
                msg_texto,
                msg_id_respuesta,
                msg_fecha
            ) VALUES (
                @msg_usu_emisor,
                @msg_usu_receptor,
                0,
                @msg_texto,
                @msg_id_respuesta,
                @fecha
            );

            DECLARE @msg_id BIGINT = CAST(SCOPE_IDENTITY() AS BIGINT);

            SELECT success = 'true', msg = 'Mensaje enviado', error = 0,
                   msg_id = @msg_id,
                   fecha = @fecha,
                   msg_id_respuesta = @msg_id_respuesta,
                   emisor_public = (SELECT usu_id_public FROM dbo.cat_usuarios
                                     WHERE usu_id = @msg_usu_emisor),
                   receptor_public = (SELECT usu_id_public FROM dbo.cat_usuarios
                                       WHERE usu_id = @msg_usu_receptor);
        END TRY
        BEGIN CATCH
            SELECT success = 'false', msg = ERROR_MESSAGE(), error = 1,
                   error_sev = ERROR_SEVERITY(), error_state = ERROR_STATE();
        END CATCH
    END

    -- ---------------------------------------------------------------
    -- 2) Marcar como leídos los mensajes de un contacto
    -- ---------------------------------------------------------------
    IF @tipoRegistro = @CHAT_MSG_READ
    BEGIN
        IF @msg_usu_lector IS NULL OR @msg_usu_otro IS NULL
        BEGIN
            SELECT success = 'false', msg = 'Lector y contacto son requeridos',
                   error = 1;
            RETURN;
        END

        BEGIN TRY
            DECLARE @fecha_lectura DATETIME = GETDATE();
            DECLARE @actualizados INT;

            UPDATE dbo.opr_chat_mensajes
               SET msg_leido = 1,
                   msg_fecha_lectura = @fecha_lectura
             WHERE msg_usu_receptor = @msg_usu_lector
               AND msg_usu_emisor = @msg_usu_otro
               AND msg_leido = 0
               AND msg_eliminado = 0
               AND (@msg_hasta IS NULL OR msg_id <= @msg_hasta);

            SET @actualizados = @@ROWCOUNT;

            SELECT success = 'true', msg = 'Mensajes marcados como leidos',
                   error = 0, actualizados = @actualizados;
        END TRY
        BEGIN CATCH
            SELECT success = 'false', msg = ERROR_MESSAGE(), error = 1,
                   error_sev = ERROR_SEVERITY(), error_state = ERROR_STATE();
        END CATCH
    END
END
GO
