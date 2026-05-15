# vault
SFTP Custom Text User Interface for Encrypted Personal Storage Servers

# REQUIREMENTS
- SFTP Server
- SSH Keyfile
- Login Passphrase

# CONFIGURATION
Write a vault.conf file inside your .config directory with this variables:

```
HOST         mydomain.duckdns.org
USER         serveruser
KEYFILE      /home/localuser/.ssh/id_ed25519
SFTP_PATH    /vault/
```
