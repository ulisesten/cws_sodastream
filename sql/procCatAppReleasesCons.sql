SET ANSI_NULLS ON
GO
SET QUOTED_IDENTIFIER ON
GO
-- =============================================
-- Author:    Ulises Martínez Elías
-- Create date: 12/08/2026
-- Description:  Para consulta versiones de la aplicación
-- =============================================
ALTER PROCEDURE [dbo].[procCatAppReleasesCons]
    @tipoConsulta    SMALLINT
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE
        @CONS_RELEASES_CONS SMALLINT = 1;

    --- Consulta para obtener todas las temporadas
    IF @tipoConsulta = @CONS_RELEASES_CONS BEGIN
        
      SELECT TOP 3
            rel_id,
            rel_version,
            rel_path,
            case when rel_type = 1 then 'Mobile' else (case when rel_type = 2 then 'TV' else 'Desktop' end) end as rel_type,
            rel_date,
            rel_description
        FROM
            cat_app_releases
        ORDER BY
            rel_date DESC;
            
    END

END
GO
