const { Router } = require("express");
const users = Router();
const usersService = require('./service/users.service')


users.post("/signin", usersService.user_signin.bind(usersService));
users.post("/", usersService.user_new.bind(usersService));

module.exports = users;