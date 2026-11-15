SET ANSI_NULLS ON
GO
SET QUOTED_IDENTIFIER ON
GO
-- ===================================================
-- Integración Telegram + mapeo externo (IP/teléfono)
-- ===================================================

IF OBJECT_ID('dbo.opr_telegram_codigos', 'U') IS NULL
BEGIN
    CREATE TABLE dbo.opr_telegram_codigos (
        tgc_id          INT IDENTITY(1,1) NOT NULL
            CONSTRAINT PK_opr_telegram_codigos PRIMARY KEY,
        tgc_id_usuario  INT NOT NULL,
        tgc_codigo      CHAR(32) NOT NULL,
        tgc_expira      DATETIME2 NOT NULL,
        tgc_usado       BIT NOT NULL CONSTRAINT DF_opr_telegram_codigos_usado DEFAULT (0),
        tgc_creado      DATETIME2 NOT NULL CONSTRAINT DF_opr_telegram_codigos_creado DEFAULT (SYSUTCDATETIME()),
        CONSTRAINT UQ_opr_telegram_codigos_codigo UNIQUE (tgc_codigo)
    );
    CREATE INDEX IX_opr_telegram_codigos_usuario
        ON dbo.opr_telegram_codigos (tgc_id_usuario, tgc_usado, tgc_expira);
END
GO

IF OBJECT_ID('dbo.opr_telegram_vinculos', 'U') IS NULL
BEGIN
    CREATE TABLE dbo.opr_telegram_vinculos (
        tgv_id           INT IDENTITY(1,1) NOT NULL
            CONSTRAINT PK_opr_telegram_vinculos PRIMARY KEY,
        tgv_id_usuario   INT NOT NULL,
        tgv_chat_id      BIGINT NOT NULL,
        tgv_usuario_tg   NVARCHAR(64) NULL,
        tgv_nombre_tg    NVARCHAR(128) NULL,
        tgv_activo       BIT NOT NULL CONSTRAINT DF_opr_telegram_vinculos_activo DEFAULT (1),
        tgv_creado       DATETIME2 NOT NULL CONSTRAINT DF_opr_telegram_vinculos_creado DEFAULT (SYSUTCDATETIME()),
        tgv_actualizado  DATETIME2 NOT NULL CONSTRAINT DF_opr_telegram_vinculos_actualizado DEFAULT (SYSUTCDATETIME())
    );
    CREATE INDEX IX_opr_telegram_vinculos_usuario
        ON dbo.opr_telegram_vinculos (tgv_id_usuario);
    CREATE INDEX IX_opr_telegram_vinculos_chat
        ON dbo.opr_telegram_vinculos (tgv_chat_id);
END
GO

IF OBJECT_ID('dbo.opr_integraciones_externas', 'U') IS NULL
BEGIN
    CREATE TABLE dbo.opr_integraciones_externas (
        ine_id           INT IDENTITY(1,1) NOT NULL
            CONSTRAINT PK_opr_integraciones_externas PRIMARY KEY,
        ine_id_usuario   INT NOT NULL,
        ine_tipo         TINYINT NOT NULL, -- 1 teléfono, 2 IP, 3 ID externo
        ine_valor        VARCHAR(255) NOT NULL,
        ine_activo       BIT NOT NULL CONSTRAINT DF_opr_integraciones_externas_activo DEFAULT (1),
        ine_creado       DATETIME2 NOT NULL CONSTRAINT DF_opr_integraciones_externas_creado DEFAULT (SYSUTCDATETIME()),
        ine_actualizado  DATETIME2 NOT NULL CONSTRAINT DF_opr_integraciones_externas_actualizado DEFAULT (SYSUTCDATETIME())
    );
    CREATE UNIQUE INDEX UQ_opr_integraciones_externas_tipo_valor
        ON dbo.opr_integraciones_externas (ine_tipo, ine_valor);
    CREATE INDEX IX_opr_integraciones_externas_usuario
        ON dbo.opr_integraciones_externas (ine_id_usuario);
END
GO

-- ===================================================
-- procTelegramProc
-- ===================================================
CREATE OR ALTER PROCEDURE dbo.procTelegramProc
    @tipoProceso     SMALLINT,
    @tgc_id_usuario  INT = NULL,
    @tgc_codigo      CHAR(32) = NULL,
    @tgc_ttl_min     INT = 15,
    @tgv_chat_id     BIGINT = NULL,
    @tgv_usuario_tg  NVARCHAR(64) = NULL,
    @tgv_nombre_tg   NVARCHAR(128) = NULL
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE
        @CREAR_CODIGO SMALLINT = 1,
        @VINCULAR     SMALLINT = 2,
        @DESVINCULAR  SMALLINT = 3;

    IF @tipoProceso = @CREAR_CODIGO
    BEGIN
        IF @tgc_id_usuario IS NULL OR @tgc_codigo IS NULL OR LEN(RTRIM(@tgc_codigo)) = 0
        BEGIN
            SELECT success = 'false', error = 1, msg = 'Usuario y código son requeridos';
            RETURN;
        END

        IF @tgc_ttl_min IS NULL OR @tgc_ttl_min < 1 OR @tgc_ttl_min > 1440
            SET @tgc_ttl_min = 15;

        -- Invalida códigos anteriores del mismo usuario.
        UPDATE dbo.opr_telegram_codigos
           SET tgc_usado = 1
         WHERE tgc_id_usuario = @tgc_id_usuario
           AND tgc_usado = 0;

        INSERT INTO dbo.opr_telegram_codigos
            (tgc_id_usuario, tgc_codigo, tgc_expira)
        VALUES
            (@tgc_id_usuario, @tgc_codigo,
             DATEADD(MINUTE, @tgc_ttl_min, SYSUTCDATETIME()));

        SELECT success = 'true', error = 0, msg = 'Código creado',
               tgc_codigo, tgc_expira 
          FROM dbo.opr_telegram_codigos
         WHERE tgc_codigo = @tgc_codigo;
        RETURN;
    END

    IF @tipoProceso = @VINCULAR
    BEGIN
        DECLARE @uid INT;

        SELECT TOP 1 @uid = tgc_id_usuario
          FROM dbo.opr_telegram_codigos
         WHERE tgc_codigo = @tgc_codigo
           AND tgc_usado = 0
           AND tgc_expira > SYSUTCDATETIME();

        IF @uid IS NULL
        BEGIN
            SELECT success = 'false', error = 1,
                   msg = 'Código inválido o expirado';
            RETURN;
        END

        IF @tgv_chat_id IS NULL
        BEGIN
            SELECT success = 'false', error = 1, msg = 'chat_id requerido';
            RETURN;
        END

        -- Un chat y un usuario tienen un solo vínculo: mueve si hace falta.
        DELETE FROM dbo.opr_telegram_vinculos
         WHERE tgv_id_usuario = @uid
            OR tgv_chat_id = @tgv_chat_id;

        INSERT INTO dbo.opr_telegram_vinculos
            (tgv_id_usuario, tgv_chat_id, tgv_usuario_tg, tgv_nombre_tg)
        VALUES
            (@uid, @tgv_chat_id, @tgv_usuario_tg, @tgv_nombre_tg);

        UPDATE dbo.opr_telegram_codigos
           SET tgc_usado = 1
         WHERE tgc_codigo = @tgc_codigo;

        SELECT success = 'true', error = 0, msg = 'Telegram vinculado',
               usu_id = @uid;
        RETURN;
    END

    IF @tipoProceso = @DESVINCULAR
    BEGIN
        IF @tgc_id_usuario IS NULL AND @tgv_chat_id IS NULL
        BEGIN
            SELECT success = 'false', error = 1,
                   msg = 'Usuario o chat_id requerido';
            RETURN;
        END

        DELETE FROM dbo.opr_telegram_vinculos
         WHERE (@tgc_id_usuario IS NOT NULL AND tgv_id_usuario = @tgc_id_usuario)
            OR (@tgv_chat_id IS NOT NULL AND tgv_chat_id = @tgv_chat_id);

        SELECT success = 'true', error = 0, msg = 'Telegram desvinculado';
        RETURN;
    END

    SELECT success = 'false', error = 1, msg = 'tipoProceso inválido';
END
GO

-- ===================================================
-- procTelegramCons
-- ===================================================
CREATE OR ALTER PROCEDURE dbo.procTelegramCons
    @tipoConsulta    SMALLINT,
    @tgc_codigo      CHAR(32) = NULL,
    @tgc_id_usuario  INT = NULL,
    @tgv_chat_id     BIGINT = NULL
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE
        @LINK_POR_USUARIO SMALLINT = 1,
        @LINK_POR_CHAT    SMALLINT = 2,
        @VALIDAR_CODIGO   SMALLINT = 3,
        @USER_PUBLIC      SMALLINT = 4;

    IF @tipoConsulta = @LINK_POR_USUARIO
    BEGIN
        SELECT TOP 1 tgv_id_usuario, tgv_chat_id, tgv_usuario_tg,
               tgv_nombre_tg, tgv_activo, tgv_actualizado
          FROM dbo.opr_telegram_vinculos
         WHERE tgv_id_usuario = @tgc_id_usuario
           AND tgv_activo = 1;
        RETURN;
    END

    IF @tipoConsulta = @LINK_POR_CHAT
    BEGIN
        SELECT TOP 1 tgv_id_usuario, tgv_chat_id, tgv_usuario_tg,
               tgv_nombre_tg, tgv_activo
          FROM dbo.opr_telegram_vinculos
         WHERE tgv_chat_id = @tgv_chat_id
           AND tgv_activo = 1;
        RETURN;
    END

    IF @tipoConsulta = @VALIDAR_CODIGO
    BEGIN
        SELECT TOP 1 tgc_id_usuario, tgc_codigo, tgc_expira, tgc_usado
          FROM dbo.opr_telegram_codigos
         WHERE tgc_codigo = @tgc_codigo
           AND tgc_usado = 0
           AND tgc_expira > SYSUTCDATETIME();
        RETURN;
    END

    IF @tipoConsulta = @USER_PUBLIC
    BEGIN
        SELECT TOP 1 usu_id_public, usu_nombre
          FROM dbo.cat_usuarios
         WHERE usu_id = @tgc_id_usuario;
        RETURN;
    END

    SELECT success = 'false', error = 1, msg = 'tipoConsulta inválido';
END
GO

-- ===================================================
-- procIntegracionesProc
-- ===================================================
CREATE OR ALTER PROCEDURE dbo.procIntegracionesProc
    @tipoProceso     SMALLINT,
    @ine_id_usuario  INT = NULL,
    @ine_tipo        TINYINT = NULL,
    @ine_valor       VARCHAR(255) = NULL
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE @LINK SMALLINT = 1, @UNLINK SMALLINT = 2;

    IF @tipoProceso = @LINK
    BEGIN
        IF @ine_id_usuario IS NULL OR @ine_tipo IS NULL
           OR @ine_valor IS NULL OR LEN(RTRIM(@ine_valor)) = 0
        BEGIN
            SELECT success = 'false', error = 1,
                   msg = 'usuario, tipo y valor son requeridos';
            RETURN;
        END

        DELETE FROM dbo.opr_integraciones_externas
         WHERE (ine_tipo = @ine_tipo AND ine_valor = @ine_valor)
            OR (ine_id_usuario = @ine_id_usuario AND ine_tipo = @ine_tipo
                AND ine_valor = @ine_valor);

        INSERT INTO dbo.opr_integraciones_externas
            (ine_id_usuario, ine_tipo, ine_valor)
        VALUES
            (@ine_id_usuario, @ine_tipo, @ine_valor);

        SELECT success = 'true', error = 0, msg = 'Mapeo guardado';
        RETURN;
    END

    IF @tipoProceso = @UNLINK
    BEGIN
        DELETE FROM dbo.opr_integraciones_externas
         WHERE (@ine_id_usuario IS NOT NULL AND ine_id_usuario = @ine_id_usuario)
           AND (@ine_tipo IS NULL OR ine_tipo = @ine_tipo)
           AND (@ine_valor IS NULL OR ine_valor = @ine_valor);

        SELECT success = 'true', error = 0, msg = 'Mapeo eliminado';
        RETURN;
    END

    SELECT success = 'false', error = 1, msg = 'tipoProceso inválido';
END
GO

-- ===================================================
-- procIntegracionesCons
-- ===================================================
CREATE OR ALTER PROCEDURE dbo.procIntegracionesCons
    @tipoConsulta    SMALLINT,
    @ine_tipo        TINYINT = NULL,
    @ine_valor       VARCHAR(255) = NULL,
    @ine_id_usuario  INT = NULL
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE @LOOKUP SMALLINT = 1, @LIST SMALLINT = 2;

    IF @tipoConsulta = @LOOKUP
    BEGIN
        SELECT TOP 1 e.ine_id_usuario, u.usu_id_public, u.usu_nombre,
               e.ine_tipo, e.ine_valor
          FROM dbo.opr_integraciones_externas e
          JOIN dbo.cat_usuarios u ON u.usu_id = e.ine_id_usuario
         WHERE e.ine_tipo = @ine_tipo
           AND e.ine_valor = @ine_valor
           AND e.ine_activo = 1;
        RETURN;
    END

    IF @tipoConsulta = @LIST
    BEGIN
        SELECT e.ine_id, e.ine_tipo, e.ine_valor, e.ine_activo, e.ine_creado
          FROM dbo.opr_integraciones_externas e
         WHERE (@ine_id_usuario IS NULL OR e.ine_id_usuario = @ine_id_usuario)
         ORDER BY e.ine_tipo, e.ine_valor;
        RETURN;
    END

    SELECT success = 'false', error = 1, msg = 'tipoConsulta inválido';
END
GO
