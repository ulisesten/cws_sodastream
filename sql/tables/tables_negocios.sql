

CREATE TABLE cat_negocios (
    neg_id INT PRIMARY KEY identity(1,1),
    neg_id_usuario INT,
    neg_nombre VARCHAR(100),
    neg_descripcion TEXT,
    neg_portada char(12),
    fecha_alta DATETIME DEFAULT CURRENT_TIMESTAMP,
    usu_alta INT,
    fecha_mod DATETIME DEFAULT CURRENT_TIMESTAMP,
    usu_mod INT
);


CREATE TABLE cat_negocios_horarios (
    hor_id INT PRIMARY KEY identity(1,1),
    hor_id_negocio INT,
    hor_descripcion TEXT,
    hor_apertura TIME,
    hor_cierre TIME,
    fecha_alta DATETIME DEFAULT CURRENT_TIMESTAMP,
    usu_alta INT,
    fecha_mod DATETIME DEFAULT CURRENT_TIMESTAMP,
    usu_mod INT
);


--SELECT
--    *
--FROM
--    cat_usuarios


-- delete from cat_usuarios where usu_id in(1012,1011,1013,1014)