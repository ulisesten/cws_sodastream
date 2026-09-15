

class UsersDTO {
    
    signinToResponse(row) {
        return {
            usu_id: row["usu_id"],
            usu_nombre: row["usu_nombre"],
            usu_correo: row["usu_correo"]
        };
    }

    signinToResponseList(rows) {
        if (!rows) return [];
        return rows.map(row => this.signinToResponse(row));
    }
}

module.exports = new UsersDTO();