const jwt = require("jsonwebtoken");
const encrypt = require("./encrypt.js");
const settings = require("./configuration");
const crypto = require("crypto");
  
const WEB_SESSION_TYPE = 1;
 
class JsonWebToken {
  secret_key = settings.getSecretKey();

  /**
   * @brief Compara dos strings de forma segura contra timing attacks
   * @param a string, primer valor a comparar
   * @param b string, segundo valor a comparar
   * @returns bool, true si son iguales
   */
  static safeCompare(a, b) {
    const bufA = Buffer.from(a);
    const bufB = Buffer.from(b);
    if (bufA.length !== bufB.length) return false;
    return crypto.timingSafeEqual(bufA, bufB);
  }

  /**
   * @brief Genera un token JWT
   * @param string Datos a codificar
   * @returns
   */

  write_token(data) {
    const credentials = settings.getCredentials();

    const fecha_exp =
      new Date().getTime() + ((settings.ACCESS_TOKEN_EXPIRATION_MINUTES || 15) * 60000);

    const payload = {
      id: credentials.usu_id,
      init: new Date().getTime(),
      exp: fecha_exp,
      user: {
        id: credentials.usu_id,
        usu_nombre: credentials.usu_nombre,
        correo: credentials.usu_correo,
      },
    };

    return jwt.sign(payload, this.secret_key, null);
  }

  /**
   * @brief Decodifica un token JWT
   * @param data string, token a decodificar
   * @param cb function, callback
   */
  verify(token, cb) {
    jwt.verify(token, this.secret_key, (err, payload) => {
      if (err) {
        cb(null, err);
        return;
      }

      cb(payload, null);
    });
  }

  /**
   * @brief Genera un token GOST
   * @param req object, datos de la petición http
   * @param data object, Datos a codificar
   * @returns string, datos codificados
   */
  write_gost_token(req, dao) {
      const credentials = dao;

      const fecha_exp = new Date().getTime() + ((settings.ACCESS_TOKEN_EXPIRATION_MINUTES || 15) * 60000);

      const payload = {
          id: credentials.usu_id,
          init: new Date().getTime(),
          exp: fecha_exp,
          session_type: WEB_SESSION_TYPE,
          user: {
              usu_id: credentials.usu_id,
              usu_nombre: credentials.usu_nombre,
              usu_correo: credentials.usu_correo,
              ip: req.ip,
              usu_salt: credentials.usu_salt
          },
          sign: encrypt.hash(this.secret_key + credentials.usu_id + req.ip)
      };

      return encrypt.reversible_encrypt(payload);
  }

  /**
   * @brief Decodifica un token GOST
   * @param data string, token a decodificar
   * @returns string
   */
  gost_verify(token) {
    try {
      const today = new Date().getTime();
      let decrypted_data = encrypt.decrypt(token);

      const decoded_data = JSON.parse(decrypted_data);

      const expected_sign = encrypt.hash(this.secret_key + decoded_data.id + decoded_data.user.ip);

      if (!JsonWebToken.safeCompare(decoded_data.sign, expected_sign)) return false;
      if (today > decoded_data.exp) return false;
      if (decoded_data.session_type != WEB_SESSION_TYPE) return false;

      return decoded_data;
    } catch (e) {
      console.error('[gost_verify] error:', e.message);
      return false;
    }
  }

  /**
   * @brief Genera un hash de una contraseña
   * @param password contraseña a encriptar
   * @returns string, hash
   */
  generate_hash(password) {
    return encrypt.reversible_encrypt(password.trim());
  }

  /**
   * @brief verifica la contraseña con el hash
   * @param password contraseña a verificar
   * @param hash hash almacenado de la contraseña
   * @returns bool si la contraseña es correcta retorna true
   */
  gost_hash_verify(password, hash) {

    const new_hash = encrypt.hash(password);

    if (hash.length != new_hash.length) return false;

    return crypto.timingSafeEqual(Buffer.from(hash), Buffer.from(new_hash));
  }

  /**
   * @brief Genera un refresh token JWT
   * @param data object, datos del usuario
   * @returns string
   */
  write_refresh_token(req, data) {
    const fecha_exp = new Date().getTime() + ((settings.REFRESH_TOKEN_EXPIRATION_DAYS || 7) * 86400000);
    const payload = {
      id: data.usu_id,
      exp: fecha_exp,
      type: 'refresh',
      session_type: WEB_SESSION_TYPE,
      user: {
        usu_id: data.usu_id,
        usu_nombre: data.usu_nombre,
        usu_correo: data.usu_correo,
        ip: req.ip,
        usu_salt: data.usu_salt
      },
      sign: encrypt.hash(this.secret_key + data.usu_id + 'refresh')
    };

    return encrypt.reversible_encrypt(payload);
  }

  /**
   * @brief Verifica un refresh token JWT
   * @param token string
   * @returns object|null
   */
  verify_refresh_token(token) {
    try {
      const today = new Date().getTime();
      let decrypted_data = encrypt.decrypt(token);
      const decoded_data = JSON.parse(decrypted_data);

      const expected_sign = encrypt.hash(this.secret_key + decoded_data.id + 'refresh');

      if (!JsonWebToken.safeCompare(decoded_data.sign, expected_sign)) return false;
      if (today > decoded_data.exp) return false;
      if (decoded_data.session_type != WEB_SESSION_TYPE) return false;

      return decoded_data;
    } catch (e) {
      console.error('[verify_refresh_token] error:', e.message);
      return false;
    }
  }

  write_csrf_token(req) {
    const exp = Date.now() + (1000 * 60 * 60 * 2);

    const payload = {
      exp,
      rand: crypto.randomBytes(32).toString('hex')
    };

    const sign = encrypt.hash(
      this.secret_key + payload.rand + payload.exp
    );

    return encrypt.reversible_encrypt({
      ...payload,
      sign
    });
  }

  write_refresh_csrf_token(req) {
    const exp = Date.now() + ((settings.REFRESH_TOKEN_EXPIRATION_DAYS || 7) * 86400000);

    const payload = {
      exp,
      rand: crypto.randomBytes(32).toString('hex'),
      type: 'refresh_csrf'
    };

    const sign = encrypt.hash(
      this.secret_key + payload.rand + payload.exp + 'refresh_csrf'
    );

    return encrypt.reversible_encrypt({
      ...payload,
      sign
    });
  }

  verify_csrf_token(token) {
    try {
      const decrypted_data = JSON.parse(encrypt.decrypt(token));

      if (Date.now() > decrypted_data.exp) return false;

      const suffix = decrypted_data.type === 'refresh_csrf' ? 'refresh_csrf' : '';
      const expected_sign = encrypt.hash(
        this.secret_key + decrypted_data.rand + decrypted_data.exp + suffix
      );

      if (!JsonWebToken.safeCompare(expected_sign, decrypted_data.sign)) return false;

      return true;
    } catch (e) {
      console.log('catch error en verify_csrf_token:', e);
      return false;
    }
  }
}

module.exports = new JsonWebToken();

// 30 * 86400000
