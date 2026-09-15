SET ANSI_NULLS ON
GO
SET QUOTED_IDENTIFIER ON
GO
-- ===================================================
-- Autor:        Ulises
-- Fecha:        2024-09-06
-- Descripción:      Procesos captura de usuarios
-- ===================================================
ALTER PROCEDURE [dbo].[procUsersProc]
    @tipoRegistro    smallint,
    @usu_nombre      nvarchar(100),
    @usu_ape_paterno nvarchar(100), 
    @usu_ape_materno nvarchar(100), 
    @usu_correo     nvarchar(150),
    @usu_contrasena nvarchar(255),
    @usu_salt       char(10),
    @fecha_alta     datetime,
    @usuario_alta   int,
    @fecha_mod      datetime,
    @usuario_mod    int,
    @error          int,
    @msg            varchar(150), 
    @error_state    int,
    @error_sev      int,
    @success        varchar(5)
AS
BEGIN TRANSACTION
    DECLARE
        @PROC_USU_REGISTRAR SMALLINT = 1

    IF @tipoRegistro = @PROC_USU_REGISTRAR BEGIN
        BEGIN TRY
                INSERT into cat_usuarios (
                    usu_nombre,
                    usu_ape_paterno,
                    usu_ape_materno,
                    usu_correo,
                    usu_contrasena,
                    usu_salt,
                    fecha_alta,
                    usuario_alta
                ) VALUES (
                    @usu_nombre,
                    @usu_ape_paterno,
                    @usu_ape_materno,
                    @usu_correo,
                    @usu_contrasena,
                    @usu_salt,
                    @fecha_alta,
                    @usuario_alta
                )

                SELECT @msg='Usuario registrado correctamente', @error = 0, @success = 'true'
        END TRY
        BEGIN CATCH  
            SELECT   @msg = ERROR_MESSAGE(), @error = @@ERROR, @error_sev = ERROR_SEVERITY(),@error_state = ERROR_STATE()
            RAISERROR(@msg,@error_sev,@error_state);
            ROLLBACK TRANSACTION
            RETURN
        END CATCH

        SELECT 
            success = @success, 
            msg= @msg, 
            error=  @error
    END

COMMIT TRANSACTION
GO
