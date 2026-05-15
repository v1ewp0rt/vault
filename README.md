# vault
SFTP Custom Text User Interface  for RSA Encrypted Personal Servers

# REQUIREMENTS
- SFTP Server
- SSH Keyfile
- Login Passphrase

# CONFIGURATION
Write a vault.conf file inside your .config directory with this variables:

```
HOST         yourhost.duckdns.org
USER         user
KEYFILE      /home/user/.ssh/id_ed25519
SFTP_PATH    /vault/
```
