SET ANSI_NULLS ON
GO
SET QUOTED_IDENTIFIER ON
GO
-- ===================================================
-- Autor:        Ulises
-- Fecha:        2025-10-04
-- Descripción:      Procesos captura de videos
-- ===================================================
ALTER PROCEDURE [dbo].[procCatVideosProc]
    @tipoRegistro    char(30),
    @vid_id          int,
    @vid_id_usuario  int,
    @vid_id_public   varchar(21),
    @vid_nombre      varchar(250),
    @vid_path        varchar(1000),
    @vid_descripcion varchar(1000),
    @vid_tags        varchar(1000),
    @vid_id_serie    int,
    @vid_temporada   SMALLINT,
    @vid_thumb_path  varchar(1000),
    @vid_id_thu_public varchar(21),
    --@vid_id_thumbnail int,
    @vid_capitulo   SMALLINT,
    @vid_id_temporada SMALLINT,

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
  
    IF @tipoRegistro = 'CAT_VIDEOS_INS' BEGIN
        BEGIN TRY
                INSERT into cat_videos (
                    vid_id_public,
                    vid_nombre,
                    vid_chapter,
                    vid_descripcion,
                    vid_path,
                    vid_id_serie,
                    vid_id_temporada,
                    vid_id_usuario,
                    vid_tags,
                    fecha_alta,
                    usuario_alta
                ) VALUES (
                    @vid_id_public,
                    @vid_nombre,
                    @vid_capitulo,
                    @vid_descripcion,
                    @vid_path,
                    @vid_id_serie,
                    @vid_id_temporada,
                    @vid_id_usuario,
                    @vid_tags,
                    @fecha_alta,
                    @usuario_alta
                )
                
                SELECT @vid_id = @@IDENTITY

                if( ISNULL(@vid_id_serie, 0) > 0) BEGIN

                    insert into opr_series_videos (
                        svi_id_serie,
                        svi_id_video,
                        svi_temporada,
                        usuario_alta,
                        fecha_alta
                    ) VALUES
                    (
                        @vid_id_serie,
                        @vid_id,
                        @vid_temporada,
                        @usuario_alta,
                        @fecha_alta
                    )

                    SELECT 
                        vid_id_thu_public = thu.thu_id_public,
                        msg = 'Video subido correctamente.',
                        error = 0,
                        success = 'true'
                    FROM
                        cat_seasons sea
                    JOIN cat_videos_thumbnails thu
                    ON
                        thu.thu_id = sea.sea_id_thumbnail
                    WHERE
                        sea_id = @vid_id_temporada;

                END

                SELECT 
                    vid_id_thu_public = null,
                    msg = 'Video subido correctamente.',
                    error = 0,
                    success = 'true'
                    

                
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
            error=  @error,
            vid_id = @vid_id,
            vid_id_thu_public = @vid_id_thu_public
    END


    IF @tipoRegistro = 'CAT_VID_THUMBNAIL_INS' BEGIN
        BEGIN TRY
                
                INSERT into cat_videos_thumbnails (
                    thu_id_video,
                    thu_path,
                    thu_id_public,
                    usuario_alta,
                    fecha_alta
                ) values(
                    @vid_id,
                    @vid_thumb_path,
                    @vid_id_thu_public,
                    @usuario_alta,
                    @fecha_alta
                )

                --SET @vid_id_thumbnail = @@IDENTITY

                UPDATE cat_videos
                SET
                    vid_thumbnail = @vid_id_thu_public,
                    fecha_mod = @fecha_mod,
                    usuario_mod = @usuario_mod
                WHERE
                    vid_id = @vid_id

                SELECT @msg='Thumbnail subido correctamente.', @error = 0, @success = 'true'
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

    IF @tipoRegistro = 'CAT_THUMBNAIL_INS' BEGIN
        BEGIN TRY
                
                INSERT into cat_videos_thumbnails (
                    thu_id_video,
                    thu_path,
                    thu_id_public,
                    usuario_alta,
                    fecha_alta
                ) values(
                    @vid_id,
                    @vid_thumb_path,
                    @vid_id_thu_public,
                    @usuario_alta,
                    @fecha_alta
                )

                SELECT @msg='Thumbnail subido correctamente.', @error = 0, @success = 'true'
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

    IF @tipoRegistro = 'CAT_VIDEOS_VIEW' BEGIN
        BEGIN TRY
                UPDATE cat_videos
                SET
                    vid_views = ISNULL(vid_views, 0) + 1
                WHERE
                    vid_id = @vid_id

                SELECT @msg='', @error = 0, @success = 'true'
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
            error=  @error,
            vid_id = @vid_id
    END


    IF @tipoRegistro = 'CAT_VIDEOS_UPDATE' BEGIN
        BEGIN TRY
            UPDATE cat_videos
            SET
                vid_nombre = @vid_nombre,
                vid_chapter = @vid_capitulo,
                vid_descripcion = @vid_descripcion,
                vid_id_serie = @vid_id_serie,
                vid_id_temporada = @vid_id_temporada,
                vid_tags = @vid_tags,
                fecha_mod = @fecha_mod,
                usuario_mod = @usuario_mod
            WHERE
                vid_id = @vid_id
            AND
                vid_id_usuario = @vid_id_usuario

            SELECT 
                @msg = 'Video actualizado correctamente.',
                @error = 0,
                @success = 'true'
                    
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
            error =  @error
    END


COMMIT TRANSACTION
GO
