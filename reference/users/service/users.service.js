const { nanoid } = require("nanoid");
//const jwt = require("../../../../../librerias/jwt/jwt.js");
const encryptService = require("../../../../../core/encrypt.js");
const authService = require("../../../../../core/authorization.js"); 
const usersDomain = require('../domain/users.domain.js');
const usersDto = require("../dto/users.dto.js");
const { reject } = require('../../../../../core/errors.js')


class UsersService {
  
    //! Create new User
    async user_new(req, res) {
        try {
            const { usu_nombre, usu_ape_paterno, usu_ape_materno, usu_correo, usu_contrasena } = req.body;

            if(!usu_correo || !usu_contrasena) {
                return reject(res, 400, 'Campos requeridos: correo, contraseña')
            }

            const hashed_email = encryptService.hash(this.normalize_email(usu_correo));
            const hashed_contrasena = encryptService.hash(usu_contrasena.trim());
            const usu_salt = nanoid(10);

            const dao = await usersDomain.users_new(
                usu_nombre,
                usu_ape_paterno,
                usu_ape_materno,
                hashed_email,
                hashed_contrasena,
                usu_salt
            )

            if (!dao) {
                return reject(res, 500, 'Error al registrar usuario');
            }

            res.status(201).json({
                success: true,
                error: 0,
                msg: 'El usuario se creó correctamente'
            });
          
        } catch (error) {
            console.error(error)
            return reject(res, 500, 'Error al registrar usuario');
        }
    }


    //! Login
    async user_signin(req, res) {
        try {

            const { usu_correo, usu_contrasena } = req.body;

            if(!usu_correo || !usu_contrasena) {
                return reject(res, 400, 'Campos requeridos: correo, contraseña')
            }

            /* const hashed_email = encryptService.hash(this.normalize_email(usu_correo));
            //const hashed_contrasena = encryptService.hash(usu_contrasena.trim());

            const dao = await usersDomain.users_signin(hashed_email)
            if (!dao) {
                return reject(res, 500, 'Error al iniciar sesión');
            } */

            const auth_service_response = await authService.user_signin( req, res );

            if (!auth_service_response) {
                return res.status(401).json({
                    success: false,
                    error: 1,
                    msg: 'El usuario no fue autorizado o no existe.'
                });
            }

            res.status(200).json({
                success: true,
                error: 0,
                msg: 'Inicio de sesión exitoso',
                data: usersDto.signinToResponse(auth_service_response)
            });

        } catch (error) {
            console.error(error)
            return reject(res, 500, 'Error al registrar usuario');
        }

    }

    normalize_email(email) {
        if (typeof email !== "string") {
            return email;
        }

        return email.trim().toLowerCase();
    }
}

module.exports = new UsersService();