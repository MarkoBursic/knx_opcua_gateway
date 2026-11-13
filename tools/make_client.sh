openssl req -x509 -nodes -days 365 -newkey rsa:2048 -keyout client_key.pem -out client_cert.pem -config openssl.cnf
