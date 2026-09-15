
const sqlEject = require("../../../../../librerias/sql_server/sql_eject.js");

const PROC_USU_REGISTRAR = 1

const CONS_USU_SIGNIN = 1

class UsersDomain {

    async users_new(
        usu_nombre, usu_ape_paterno, usu_ape_materno, usu_correo, usu_contrasena, usu_salt) {

        const parametros = {
            tipoRegistro: PROC_USU_REGISTRAR,
            usu_nombre: usu_nombre,
            usu_ape_paterno: usu_ape_paterno,
            usu_ape_materno: usu_ape_materno,
            usu_correo: usu_correo,
            usu_contrasena: usu_contrasena,
            usu_salt: usu_salt
        };
        
        return await sqlEject.store_eject("procUsersProc", parametros, "soda_stream");

    }


    async users_signin( usu_correo ) {

        const parametros = {
            tipoConsulta: CONS_USU_SIGNIN,
            usu_correo: usu_correo
        };

        return await sqlEject.store_eject( "procUsersCons", parametros, "soda_stream" );
    }
}

module.exports = new UsersDomain()