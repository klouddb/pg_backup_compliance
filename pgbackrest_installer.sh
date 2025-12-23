#!/bin/bash
# # set -e # Exit immediately if a command exits with a non-zero status

# # # Idea here is to put a tracker before pgbackrest binary to monitor its usage
# # # and log it somewhere for analysis

# # # This is a simple script to run a few commands

# # # Compile the C program for pgbackrest_monitor
# # gcc -o pgbackrest pgbackrest_wrapper.c

# # # Pushing the compiled binary to /
# # sudo mv pgbackrest /usr/local/bin/pgbackrest

# # # Make the binary executable and varous permission changes
# # sudo chmod +x /usr/local/bin/pgbackrest
# # sudo chmod 755 /usr/local/bin/pgbackrest
# # sudo chown root:postgres /usr/local/bin/pgbackrest


# # # Make a log file to store the logs
# # sudo touch /var/log/pgbackrest_wrapper_1.log
# # sudo chmod 666 /var/log/pgbackrest_wrapper_1.log
# # sudo chown postgres:postgres /var/log/pgbackrest_wrapper_1.log

# # # Put wrapper before the actual pgbackrest binary
# # PATH=/usr/local/bin:$PATH
# # export PATH

# # # reload
# # # source ~/.bashrc   


# # echo "Script finished!"

# #!/bin/bash
# set -e


# DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )"/.. && pwd )"

# # cd "$DIR"

# echo "Compiling pgbackrest wrapper..."

# gcc -o pgbackrest pgbackrest_wrapper.c

# chmod 755 pgbackrest

# echo "Wrapper compiled successfully in:"
# echo "-> $DIR/pgbackrest"

# WRAPPER_SRC="./pgbackrest"         # compiled wrapper file name
# WRAPPER_DEST="/usr/local/bin/pgbackrest"   # where wrapper will live
# LOG_DIR="/var/log"
# LOG_FILE="$LOG_DIR/pgbackrest_wrapper_1.log"
# SUDOERS_FILE="/etc/sudoers.d/pgbackrest_wrapper"
# PROFILE_FILE="/etc/profile.d/pgbackrest_wrapper.sh"

# echo "=== pgBackRest Wrapper Installer ==="

# # -----------------------------
# # 1. Check wrapper binary exists
# # -----------------------------
# if [ ! -f "$WRAPPER_SRC" ]; then
#     echo "ERROR: Wrapper binary '$WRAPPER_SRC' not found."
#     echo "Compile using: gcc -o pgbackrest pgbackrest_wrapper.c"
#     exit 1
# fi

# # -----------------------------
# # 2. Detect postgres user/group
# # -----------------------------
# POSTGRES_USER=""
# for u in postgres pgsql postgresql; do
#     if id "$u" >/dev/null 2>&1; then
#         POSTGRES_USER="$u"
#         break
#     fi
# done

# if [ -z "$POSTGRES_USER" ]; then
#     echo "ERROR: No postgres user found (postgres/pgsql/postgresql)."
#     exit 1
# fi

# POSTGRES_GROUP=$(id -gn "$POSTGRES_USER")

# echo "Detected postgres user:  $POSTGRES_USER"
# echo "Detected postgres group: $POSTGRES_GROUP"

# # -----------------------------
# # 3. Create log directory safely
# # -----------------------------
# echo "Creating log directory..."

# sudo mkdir -p "$LOG_DIR"
# sudo touch "$LOG_FILE"
# sudo chown $POSTGRES_USER:$POSTGRES_GROUP "$LOG_FILE"
# sudo chmod 640 "$LOG_FILE"

# echo "Log file: $LOG_FILE"

# # -----------------------------
# # 4. Backup existing wrapper
# # -----------------------------
# if [ -f "$WRAPPER_DEST" ]; then
#     TS=$(date +%Y%m%d_%H%M%S)
#     BACKUP="${WRAPPER_DEST}.backup_$TS"
#     echo "Existing pgbackrest found at $WRAPPER_DEST"
#     echo "Backing it up to $BACKUP"
#     sudo mv "$WRAPPER_DEST" "$BACKUP"
# fi

# # -----------------------------
# # 5. Install new wrapper
# # -----------------------------
# echo "Installing wrapper into $WRAPPER_DEST..."
# sudo mv "$WRAPPER_SRC" "$WRAPPER_DEST"
# sudo chmod 755 "$WRAPPER_DEST"
# sudo chown root:$POSTGRES_GROUP "$WRAPPER_DEST"

# echo "Wrapper installed."

# # -----------------------------
# # 6. Ensure wrapper is FIRST in PATH
# # -----------------------------
# echo "Configuring PATH for all users and services..."

# # Profile entry
# sudo bash -c "cat > $PROFILE_FILE" <<EOF
# # pgBackRest wrapper PATH override
# export PATH="/usr/local/bin:\$PATH"
# EOF

# sudo chmod 644 "$PROFILE_FILE"

# # Sudo secure path
# if [ ! -f "$SUDOERS_FILE" ]; then
#     echo "Updating sudoers secure_path..."
#     sudo bash -c "echo 'Defaults secure_path=\"/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin\"' > $SUDOERS_FILE"
#     sudo chmod 440 "$SUDOERS_FILE"
# fi

# # -----------------------------
# # 7. Reload environment for user
# # -----------------------------
# echo "Reloading /etc/profile.d scripts..."
# source "$PROFILE_FILE" || true

# # -----------------------------
# # 8. Verification
# # -----------------------------
# echo "Verifying installation..."

# REAL=$(which -a pgbackrest | sed -n '2p')
# WRAPPER=$(which -a pgbackrest | sed -n '1p')

# echo "Wrapper path: $WRAPPER"
# echo "Real binary candidate: $REAL"

# if [[ "$WRAPPER" == "$WRAPPER_DEST" ]]; then
#     echo "SUCCESS: Wrapper is first in PATH."
# else
#     echo "WARNING: Wrapper is NOT first in PATH!"
#     echo "Please ensure /usr/local/bin is before other directories."
# fi

# echo "=== INSTALLATION COMPLETE ==="

# set -e


# DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )"/.. && pwd )"

# # cd "$DIR"

# echo "Compiling pgbackrest wrapper..."

# gcc pgbackrest_wrapper.c \
#   -I$(pg_config --includedir) \
#   -L$(pg_config --libdir) \
#   -lpq \
#   -o pgbackrest


# chmod 755 pgbackrest

# echo "Wrapper compiled successfully in:"
# echo "-> $DIR/pgbackrest"

# WRAPPER_SRC="./pgbackrest"         # compiled wrapper file name
# WRAPPER_DEST="/usr/local/bin/pgbackrest"   # where wrapper will live
# LOG_DIR="/var/log"
# LOG_FILE="$LOG_DIR/pgbackrest_wrapper_1.log"
# SUDOERS_FILE="/etc/sudoers.d/pgbackrest_wrapper"
# PROFILE_FILE="/etc/profile.d/pgbackrest_wrapper.sh"

# echo "=== pgBackRest Wrapper Installer ==="

# # -----------------------------
# # 1. Check wrapper binary exists
# # -----------------------------
# if [ ! -f "$WRAPPER_SRC" ]; then
#     echo "ERROR: Wrapper binary '$WRAPPER_SRC' not found."
#     echo "Compile using: gcc -o pgbackrest pgbackrest_wrapper.c"
#     exit 1
# fi

# # -----------------------------
# # 2. Detect postgres user/group
# # -----------------------------
# POSTGRES_USER=""
# for u in postgres pgsql postgresql; do
#     if id "$u" >/dev/null 2>&1; then
#         POSTGRES_USER="$u"
#         break
#     fi
# done

# if [ -z "$POSTGRES_USER" ]; then
#     echo "ERROR: No postgres user found (postgres/pgsql/postgresql)."
#     exit 1
# fi

# POSTGRES_GROUP=$(id -gn "$POSTGRES_USER")

# echo "Detected postgres user:  $POSTGRES_USER"
# echo "Detected postgres group: $POSTGRES_GROUP"

# # -----------------------------
# # 3. Create log directory safely
# # -----------------------------
# echo "Creating log directory..."

# sudo mkdir -p "$LOG_DIR"
# sudo touch "$LOG_FILE"
# sudo chown $POSTGRES_USER:$POSTGRES_GROUP "$LOG_FILE"
# sudo chmod 640 "$LOG_FILE"

# echo "Log file: $LOG_FILE"

# # -----------------------------
# # 4. Backup existing wrapper
# # -----------------------------
# if [ -f "$WRAPPER_DEST" ]; then
#     TS=$(date +%Y%m%d_%H%M%S)
#     BACKUP="${WRAPPER_DEST}.backup_$TS"
#     echo "Existing pgbackrest found at $WRAPPER_DEST"
#     echo "Backing it up to $BACKUP"
#     sudo mv "$WRAPPER_DEST" "$BACKUP"
# fi

# # -----------------------------
# # 5. Install new wrapper
# # -----------------------------
# echo "Installing wrapper into $WRAPPER_DEST..."
# sudo mv "$WRAPPER_SRC" "$WRAPPER_DEST"
# sudo chmod 755 "$WRAPPER_DEST"
# sudo chown root:$POSTGRES_GROUP "$WRAPPER_DEST"

# echo "Wrapper installed."

# # -----------------------------
# # 6. Ensure wrapper is FIRST in PATH
# # -----------------------------
# echo "Configuring PATH for all users and services..."

# # Profile entry
# sudo bash -c "cat > $PROFILE_FILE" <<EOF
# # pgBackRest wrapper PATH override
# export PATH="/usr/local/bin:\$PATH"
# EOF

# sudo chmod 644 "$PROFILE_FILE"

# # Sudo secure path
# if [ ! -f "$SUDOERS_FILE" ]; then
#     echo "Updating sudoers secure_path..."
#     sudo bash -c "echo 'Defaults secure_path=\"/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin\"' > $SUDOERS_FILE"
#     sudo chmod 440 "$SUDOERS_FILE"
# fi

# # -----------------------------
# # 7. Reload environment for user
# # -----------------------------
# echo "Reloading /etc/profile.d scripts..."
# source "$PROFILE_FILE" || true

# # -----------------------------
# # 8. Verification
# # -----------------------------
# echo "Verifying installation..."

# REAL=$(which -a pgbackrest | sed -n '2p')
# WRAPPER=$(which -a pgbackrest | sed -n '1p')

# echo "Wrapper path: $WRAPPER"
# echo "Real binary candidate: $REAL"

# if [[ "$WRAPPER" == "$WRAPPER_DEST" ]]; then
#     echo "SUCCESS: Wrapper is first in PATH."
# else
#     echo "WARNING: Wrapper is NOT first in PATH!"
#     echo "Please ensure /usr/local/bin is before other directories."
# fi

# echo "=== INSTALLATION COMPLETE ==="


#!/bin/bash
set -e

# Helper to handle sudo only if not root
run_cmd() {
    if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        "$@"
    fi
}

echo "=== Compiling pgBackRest Wrapper ==="
gcc pgbackrest_wrapper.c \
  -I$(pg_config --includedir) \
  -L$(pg_config --libdir) \
  -lpq \
  -o pgbackrest_bin

# Configuration
WRAPPER_DEST="/usr/local/bin/pgbackrest"
PROFILE_FILE="/etc/profile.d/pgbackrest_wrapper.sh"
SUDOERS_FILE="/etc/sudoers.d/pgbackrest_wrapper"

echo "=== Installing Wrapper ==="

# 1. Handle existing file (Move/Backup)
if [ -f "$WRAPPER_DEST" ]; then
    TS=$(date +%Y%m%d_%H%M%S)
    echo "Found existing binary at $WRAPPER_DEST. Moving to backup."
    run_cmd mv "$WRAPPER_DEST" "${WRAPPER_DEST}.backup_$TS"
fi

# 2. Place new wrapper
run_cmd mkdir -p "/usr/local/bin"
run_cmd mv pgbackrest_bin "$WRAPPER_DEST"
run_cmd chmod 755 "$WRAPPER_DEST"
run_cmd chown root:$(id -gn postgres 2>/dev/null || echo root) "$WRAPPER_DEST"

# 3. PATH Priority (Only add if not already there)
# This prevents 'which -a' from showing duplicate /usr/local/bin entries
if [[ ":$PATH:" != *":/usr/local/bin:"* ]]; then
    echo "Configuring PATH priority..."
    run_cmd mkdir -p /etc/profile.d
    run_cmd bash -c "echo 'export PATH=\"/usr/local/bin:\$PATH\"' > $PROFILE_FILE"
    run_cmd chmod 644 "$PROFILE_FILE"
fi

# 4. Sudoers secure_path (Only if directory exists)
if [ -d "/etc/sudoers.d" ]; then
    run_cmd bash -c "echo 'Defaults secure_path=\"/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin\"' > $SUDOERS_FILE"
    run_cmd chmod 440 "$SUDOERS_FILE"
fi

echo "=== Verification ==="
# Reload current session path for verification
export PATH="/usr/local/bin:$PATH"
which -a pgbackrest

echo "=== INSTALLATION COMPLETE ==="