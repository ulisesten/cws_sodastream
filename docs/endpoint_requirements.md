Añadir el siguiente endpoint protegido

https://cws.sodastream.fun/api/v1/videos/external

Esta es una migración del endpoint en streaming_server, posteriormente hay que apuntar el front de streaming_server al nuevo endpoint

[POST] /api/v1/videos/external

### Rama del sp [procCatVideosProc]

```sql
IF @tipoRegistro = 'CAT_VIDEOS_EXTERNAL_INS' BEGIN
        BEGIN TRY

                SELECT
                    @vid_id_thu_public = thu.thu_id_public
                FROM
                    cat_videos_thumbnails thu
                WHERE
                    thu_id = @vid_id_thumbnail

                INSERT into cat_videos (
                    vid_id_public, --- nanoid generated id varchar(11)
                    vid_nombre,
                    vid_path,
                    vid_thumbnail,  --- varchar(11)
                    vid_id_usuario, --- int
                    vid_tags,
                    fecha_alta,
                    usuario_alta
                ) VALUES (
                    @vid_id_public,
                    @vid_nombre,
                    @vid_path,
                    @vid_id_thu_public,
                    @vid_id_usuario,
                    @vid_tags,
                    @fecha_alta,
                    @usuario_alta
                )
                
                SELECT @vid_id = @@IDENTITY

                SELECT 
                    vid_id_thu_public = @vid_id_thu_public,
                    msg = 'Video Externo subido correctamente.',
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
```



