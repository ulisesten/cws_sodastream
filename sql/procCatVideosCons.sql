SET ANSI_NULLS ON
GO
SET QUOTED_IDENTIFIER ON
GO
-- =============================================
-- Author:    Ulises Martínez Elías
-- Create date: 04/10/2025
-- Description:  Para consulta videos
-- =============================================
ALTER PROCEDURE [dbo].[procCatVideosCons]
    @tipoConsulta    SMALLINT,
    @vid_id_public  varchar(21),
    @thu_id_public  varchar(21),
    @vid_id         int,
    @ser_id         int,

    @usuario_alta   int
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE @CAT_SERIES_VIDEOS_CONS SMALLINT = 1;
    DECLARE @CAT_VIDEO_BY_ID_CONS SMALLINT = 2;
    DECLARE @CAT_VID_THUMBNAIL_CONS SMALLINT = 3;
    DECLARE @CAT_VIDEOS_CONS SMALLINT = 4;
    DECLARE @CAT_VIDEOS_TABLE_FORMAT_CONS SMALLINT = 5;
    DECLARE @CAT_THUMBNAILS_CONS SMALLINT = 6;
    DECLARE @CAT_VIDEOS_MAS_VISTOS_CONS SMALLINT = 7;


    IF @tipoConsulta = @CAT_VIDEOS_CONS BEGIN
        
        DECLARE @tbl_order_videos AS TABLE(
                    vid_id int,
                    vid_tipo SMALLINT,
                    vid_thumbnail   varchar(11)
                )

        --! Videos por serie
        INSERT into @tbl_order_videos(
            vid_id,
            vid_tipo,
            vid_thumbnail
        ) SELECT DISTINCT
            MAX(vid.vid_id),
            2, --- Serie,
            thu.thu_id_public AS vid_thumbnail
        FROM
            cat_videos vid
        LEFT JOIN cat_seasons sea
        ON
            sea.sea_id = vid.vid_id_temporada
        LEFT JOIN cat_videos_thumbnails thu
        ON
            thu.thu_id = sea.sea_id_thumbnail
        WHERE
            vid.vid_tipo > 0
        AND
            isnull(vid.vid_id_serie,0) > 0
        GROUP BY
            sea.sea_numero,
            thu.thu_id_public


        --! Videos individuales(que no pertenecen a una serie)
        INSERT INTO @tbl_order_videos (
            vid_id,
            vid_tipo,
            vid_thumbnail
        ) SELECT
            vid.vid_id,
            1, --- Video
            vid.vid_thumbnail
        FROM
            cat_videos vid
        LEFT JOIN
            opr_series_videos ser
        ON
            ser.svi_id_video = vid.vid_id
        WHERE
            vid.vid_tipo > 0
        AND
            ser.svi_id_video IS NULL
        AND
            isnull(vid.vid_id_serie,0) = 0
        GROUP BY
            vid.vid_id,
            vid.vid_thumbnail


        SELECT
            vid.vid_id,
            --vid_id_usuario,
            vid_id_public,
            vid_nombre,
            ord.vid_tipo,
            --vid_path,
            --vid_descripcion,
            --vid_likes,
            --vid_dislikes,
            ord.vid_thumbnail,
            fecha_alta AS vid_fecha
        FROM
            @tbl_order_videos ord
        JOIN
            cat_videos vid
        on
            vid.vid_id = ord.vid_id
        ORDER BY
            fecha_alta DESC
            
    END

    IF @tipoConsulta = @CAT_SERIES_VIDEOS_CONS BEGIN

        SELECT TOP 1
            @ser_id = vid_id_serie
        FROM
            cat_videos
        WHERE
            vid_id = @vid_id

        SELECT
            vid_id,
            --vid_id_usuario,
            vid_id_public,
            vid_nombre,
            --vid_path,
            --vid_descripcion,
            --vid_likes,
            --vid_dislikes,
            --vid_views,
            sea_numero AS vid_temporada,
            ser.ser_nombre AS vid_serie,
            vid_thumbnail,
            vid_chapter,
            vid.fecha_alta AS vid_fecha
        FROM
            cat_videos vid
        JOIN
            cat_seasons sea
        ON
            vid.vid_id_temporada = sea.sea_id
        JOIN
            cat_series ser
        ON
            vid.vid_id_serie = ser.ser_id
        WHERE

            vid.vid_id_serie = @ser_id
            
    END

    IF @tipoConsulta = @CAT_VIDEO_BY_ID_CONS BEGIN

        SELECT
            vid_id,
            vid_id_usuario,
            vid_id_public,
            vid_nombre,
            vid_path,
            vid_descripcion,
            vid_likes,
            vid_dislikes,
            vid_views,
            vid_thumbnail,
            vid_tipo,
            isnull(vid_id_serie, 0) AS vid_id_serie,
            isnull(vid_chapter, 0) AS vid_chapter,
            isnull(vid_id_temporada, 0) AS vid_id_temporada,
            vid.fecha_alta AS vid_fecha
        FROM
            cat_videos vid
        WHERE
            vid_id_public = @vid_id_public
            
    END

    IF @tipoConsulta = @CAT_VID_THUMBNAIL_CONS BEGIN

        SELECT
            'Se obtuvo el thumbnail correctamente.'   AS msg,
            'True'      AS success,
             0          AS error,
            thu_id,
            thu_id_video,
            thu_path,
            fecha_alta AS thu_fecha
        FROM
            cat_videos_thumbnails
        WHERE
            thu_id_public = @thu_id_public
            
    END


    IF @tipoConsulta = @CAT_THUMBNAILS_CONS BEGIN

        SELECT
            thu_id,
            thu_id_video,
            thu_path,
            fecha_alta AS thu_fecha
        FROM
            cat_videos_thumbnails
        --WHERE
        --    usuario_alta = @usuario_alta
            
    END

    IF @tipoConsulta = @CAT_VIDEOS_TABLE_FORMAT_CONS BEGIN

        SELECT
            vid_id,
            vid_id_public,
            vid_nombre,
            vid_chapter,
            vid_likes,
            vid_dislikes,
            vid_views,
            vid_descripcion,
            vid_id_serie,
            vid_id_temporada,
            vid_tags,
            vid_tipo,
            fecha_alta AS vid_fecha
        FROM
            cat_videos
        WHERE
            usuario_alta = @usuario_alta
        ORDER BY fecha_alta DESC
            
    END


    IF @tipoConsulta = @CAT_VIDEOS_MAS_VISTOS_CONS BEGIN
        --- Testing query videos
        --DECLARE @tbl_order_videos AS TABLE(
        --            vid_id int,
        --            vid_tipo SMALLINT,
        --            vid_thumbnail   varchar(11)
        --        )

        --! Videos por serie
        INSERT into @tbl_order_videos(
            vid_id,
            vid_tipo,
            vid_thumbnail
        ) SELECT DISTINCT TOP 10
            MAX(vid.vid_id),
            2, --- Serie,
            thu.thu_id_public AS vid_thumbnail
        FROM
            cat_videos vid
        LEFT JOIN cat_seasons sea
        ON
            sea.sea_id = vid.vid_id_temporada
        LEFT JOIN cat_videos_thumbnails thu
        ON
            thu.thu_id = sea.sea_id_thumbnail
        WHERE
            vid.vid_tipo > 0
        AND
            isnull(vid.vid_id_serie,0) > 0
        GROUP BY
            vid_views,
            sea.sea_numero,
            thu.thu_id_public


        --! Videos individuales(que no pertenecen a una serie)
        INSERT INTO @tbl_order_videos (
            vid_id,
            vid_tipo,
            vid_thumbnail
        ) SELECT TOP 10
            vid.vid_id,
            1, --- Video
            vid.vid_thumbnail
        FROM
            cat_videos vid
        LEFT JOIN
            opr_series_videos ser
        ON
            ser.svi_id_video = vid.vid_id
        WHERE
            vid.vid_tipo > 0
        AND
            ser.svi_id_video IS NULL
        AND
            isnull(vid.vid_id_serie,0) = 0
        GROUP BY
            vid.vid_views,
            vid.vid_id,
            vid.vid_thumbnail


        SELECT TOP 10
            vid.vid_id,
            --vid_id_usuario,
            vid_id_public,
            vid_nombre,
            ord.vid_tipo,
            --vid_path,
            --vid_descripcion,
            --vid_likes,
            --vid_dislikes,
            ord.vid_thumbnail,
            fecha_alta AS vid_fecha
        FROM
            @tbl_order_videos ord
        JOIN
            cat_videos vid
        on
            vid.vid_id = ord.vid_id
        ORDER BY
            vid.vid_views DESC,
            fecha_alta DESC
            
    END

END
GO
