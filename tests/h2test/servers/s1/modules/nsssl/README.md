# TLS material for local tests

`cert.pem` and `key.pem` are **not** tracked in git. Generate them with:

```sh
tests/h2test/generate-tls-certs.sh
```

`run-h2spec.sh --start-nsd` and `run-h3spec.sh --start-nsd` run this automatically before starting **nsd**.
