SET ANSI_NULLS ON
GO
SET QUOTED_IDENTIFIER ON
GO
-- =============================================
-- Author:    Isnaya Andrade Reyes
-- Create date: 06/09/2024
-- Description:  Para consulta usuarios
-- =============================================
ALTER PROCEDURE [dbo].[procUsersCons]
    @tipoConsulta    smallint,
    @usu_correo     nvarchar(150),
    @usu_id         int,
    @usu_id_public  char(12)
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE
        @CONS_USU_SIGNIN smallint = 1,
        @USUARIO_INFO_CONS smallint = 2,
        @USU_BY_ID_PUBLIC_CONS smallint = 3,
        @USU_LIST_CONS smallint = 4,
        @USU_BY_ID_CONS smallint = 5

    -- IF @tipoConsulta = 'USUARIOS_CONS' BEGIN

    --     SELECT
    --         'Se obtuvieron los usuarios correctamente.'   AS msg,
    --         'True'      AS success,
    --          0          AS error,
    --         *
    --     FROM
    --         cat_usuarios
            
    -- END

    IF @tipoConsulta = @CONS_USU_SIGNIN BEGIN

        SELECT
            usu_id,
            usu_correo,
            usu_nombre,
            usu_contrasena,
            usu_salt
        FROM
            cat_usuarios
        WHERE
            usu_correo = @usu_correo

    END

    IF @tipoConsulta = @USUARIO_INFO_CONS BEGIN

        SELECT TOP 1
            usu_id,
            usu_correo,
            usu_nombre
        FROM
            cat_usuarios
        WHERE
            usu_id = @usu_id

    END


    IF @tipoConsulta = @USU_BY_ID_PUBLIC_CONS BEGIN

        SELECT TOP 30
            usu_id,
            usu_nombre
        FROM
            cat_usuarios
        WHERE
            usu_id_public = @usu_id_public

    END

    -- Identificador público actual del usuario (setup idempotente).
    IF @tipoConsulta = @USU_BY_ID_CONS BEGIN

        SELECT TOP 1
            usu_id,
            usu_id_public
        FROM
            cat_usuarios
        WHERE
            usu_id = @usu_id

    END

    -- Lista de usuarios con mensajería habilitada (id público de 12).
    -- @usu_id se usa para excluir al propio usuario.
    IF @tipoConsulta = @USU_LIST_CONS BEGIN

        SELECT
            usu_id,
            usu_id_public,
            usu_nombre
        FROM
            cat_usuarios
        WHERE
            usu_id_public IS NOT NULL
            AND LEN(LTRIM(RTRIM(usu_id_public))) = 12
            AND (@usu_id IS NULL OR usu_id <> @usu_id)
        ORDER BY
            usu_nombre, usu_id

    END
END
GO
