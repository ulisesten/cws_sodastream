//const { path, settings } = require("../../../server.js");
const jwt = require("./jwt.js");
const settings = require("./configuration.js");
const sqlEject = require("./sql_eject.js");
const { reject } = require("./errors.js");
const encryptService = require("./encrypt.js");
//const url_refresh_token = settings.SERVER_HOST + '/api/v1/users/refresh_token`';

const CONS_USU_SIGNIN = 1

class AuthorizationService {

    async userDAO(prm_usu_correo) {
        const usu_correo = prm_usu_correo.trim().toLowerCase();
        const parametros = {
            tipoConsulta: CONS_USU_SIGNIN,
            usu_correo: usu_correo
        };
        
        return await sqlEject.store_eject( "procUsersCons", parametros, "soda_stream" );
    }

    async verify(req, res, next) {
        const token = req.cookies['access_token'];
        const csrf_token = req.headers['x-csrf-token'];

        if ( !token  || !csrf_token) {
            res.status(401).json({
                success: false,
                error: 1,
                msg: 'No credentials are present.',
                status: 401
            });
            res.end()
            return;
        }

        const auth = jwt.gost_verify(token);
        const csrf_valid = jwt.verify_csrf_token(csrf_token);

        if (!auth || !csrf_valid) {
            console.error('[AUTH verify] rechazado - gost:', !!auth, 'csrf:', csrf_valid, 'path:', req.path);
            res.status(401).json({
                success: false,
                error: 1,
                msg: 'Authentication rejected.',
                status: 401
            });
            res.end();
            return;
        }

        const dao = (await this.userDAO(auth.user.usu_correo))[0];
        if (!dao || dao.length == 0) {
            return reject(res, 500, 'Error al iniciar sesión');
        }

        if( !auth.user.usu_salt || dao.usu_salt.trim() !== auth.user.usu_salt.trim()){
            console.log('[ERROR]error de salt', auth)
            return reject(res, 401, 'Error al iniciar sesión');
        }

        req.user = auth.user;
        req.authorized = true;

        next();
    }

    async user_signin( req, res ) {
        const password = req.body['usu_contrasena'];

        const hashed_email = encryptService.hash(req.body.usu_correo.trim().toLowerCase())
        const dao = (await this.userDAO(hashed_email))[0];
        if ( !dao )
            return null;

        const hash = dao["usu_contrasena"];

        if (!jwt.gost_hash_verify(password, hash)) {
            return null
        }

        const access_token = jwt.write_gost_token(req, dao);
        const refresh_token = jwt.write_refresh_token(req, dao);
        const csrf_token = jwt.write_csrf_token(req);
        const refresh_csrf_token = jwt.write_refresh_csrf_token(req);

        const isProduction = settings.NODE_ENV === 'production';
        const refreshMaxAge = (settings.REFRESH_TOKEN_EXPIRATION_DAYS || 7) * 86400000;

        /// Tokens
        res.cookie('access_token', access_token, {
            httpOnly: true,
            secure: isProduction,
            sameSite: 'Lax',
            path: '/api/v1',
            maxAge: (settings.ACCESS_TOKEN_EXPIRATION_MINUTES || 15) * 60000
        });

        res.cookie('csrf_token', csrf_token, {
            secure: isProduction,
            sameSite: 'Lax',
            path: '/',
            maxAge: (settings.ACCESS_TOKEN_EXPIRATION_MINUTES || 15) * 60000
        });

        /// Refresh tokens
        res.cookie('refresh_token', refresh_token, {
            httpOnly: true,
            secure: isProduction,
            sameSite: 'Lax',
            path: '/api/v1/users/refresh_token',
            maxAge: refreshMaxAge
        });

        res.cookie('refresh_csrf_token', refresh_csrf_token, {
            secure: isProduction,
            sameSite: 'Lax',
            path: '/',
            maxAge: refreshMaxAge
        });

        console.log(dao)

        return {
            usu_id: dao["usu_id"],
            usu_nombre: dao["usu_nombre"],
            usu_correo: req.body.usu_correo.trim(),
            usu_salt: dao["usu_salt"]
        };

    }

    async refresh(req, res, next) {
        const token = req.cookies['refresh_token'];
        const refresh_csrf_token = req.headers['x-csrf-token'];

        if (!token || !refresh_csrf_token) {
            res.status(401).json({
                success: false,
                error: 1,
                msg: 'No credentials are present.'
            });
            return;
        }

        const auth = jwt.verify_refresh_token(token);
        const csrf_valid = jwt.verify_csrf_token(refresh_csrf_token);

        if (!auth || !csrf_valid) {
            res.status(401).json({
                success: false,
                error: 1,
                msg: 'Authentication rejected.'
            });
            return;
        }

        const dao = (await this.userDAO(auth.user.usu_correo))[0];
        if (!dao || dao.length == 0) {
            return reject(res, 500, 'Error al iniciar sesión');
        }

        if( !auth.user.usu_salt || dao.usu_salt.trim() !== auth.user.usu_salt.trim()){
            console.log('[ERROR] user_salt no coincide', dao.usu_id)
            return reject(res, 401, 'Error al iniciar sesión');
        }

        req.user = auth.user;
        req.authorized = true;

        const user = {
            usu_id: auth.user.usu_id,
            usu_nombre: auth.user.usu_nombre,
            usu_correo: auth.user.usu_correo,
            usu_salt: auth.user.usu_salt
        }

        const new_access_token = jwt.write_gost_token(req, user);
        const new_csrf_token = jwt.write_csrf_token(req);
        const new_refresh_csrf_token = jwt.write_refresh_csrf_token(req);

        const isProduction = settings.NODE_ENV === 'production';

        res.cookie('access_token', new_access_token, {
            httpOnly: true,
            secure: isProduction,
            sameSite: 'Lax',
            path: '/api/v1',
            maxAge: (settings.ACCESS_TOKEN_EXPIRATION_MINUTES || 15) * 60000
        });

        res.cookie('csrf_token', new_csrf_token, {
            secure: isProduction,
            sameSite: 'Lax',
            path: '/',
            maxAge: (settings.ACCESS_TOKEN_EXPIRATION_MINUTES || 15) * 60000
        });

        res.cookie('refresh_csrf_token', new_refresh_csrf_token, {
            secure: isProduction,
            sameSite: 'Lax',
            path: '/',
            maxAge: (settings.REFRESH_TOKEN_EXPIRATION_DAYS || 7) * 86400000
        });

        next();
    }

    /**
     * @deprecated 
     */
    authorize(req, res, next) {
        const authHeader = req.headers['authorization'];
        const authCookie = req.cookies['access_token'];
        const token = null;

        if (!authHeader && !authCookie) {
            res.json({
                success: false,
                error: 1,
                msg: 'No credentials are present.'
            });
            return;
        }

        if (authHeader && !authHeader.startsWith('Bearer ')) {
            res.json({
                success: false,
                error: 1,
                msg: 'No valid token.'
            });
            return;
        }

        
        token = authHeader? authHeader.split(' ')[1] : authCookie;
        
        let auth = jwt.gost_verify(token);

        if (req.ip !== auth.user['ip']) {
            auth = null;
        }

        if (!auth) {
            res.json({
                success: false,
                error: 1,
                msg: 'Authentication error.'
            });
            req.authorized = false;
            req.user = null; // Si no se encuentra el token, asignamos null
            return;
        }

        req.user = auth;
        req.authorized = true;

        next();
    }
}

module.exports = new AuthorizationService();
