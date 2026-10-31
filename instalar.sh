#!/usr/bin/env bash
# Script de instalação do driver ms912x com suporte a DKMS
set -e

if [ "$EUID" -ne 0 ]; then
    echo "Erro: execute este script com permissões de administrador."
    echo "Uso: sudo bash $0"
    exit 1
fi

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KVER="$(uname -r)"

echo "==> [1/4] Compilando módulo para o kernel atual ($KVER)..."
make -C "$DIR" clean
make -C "$DIR" all -j"$(nproc)"

echo "==> [2/4] Copiando módulo e atualizando dependências..."
mkdir -p "/lib/modules/$KVER/updates"
cp "$DIR/ms912x.ko" "/lib/modules/$KVER/updates/"
depmod -a "$KVER"

echo "==> [3/4] Registrando no DKMS (recompilação automática em updates de kernel)..."
mkdir -p /usr/src/ms912x-0.1
cp -r "$DIR"/* /usr/src/ms912x-0.1/
make -C /usr/src/ms912x-0.1 clean 2>/dev/null || true
rm -f /usr/src/ms912x-0.1/*.o /usr/src/ms912x-0.1/*.ko /usr/src/ms912x-0.1/.*.cmd /usr/src/ms912x-0.1/*.mod*

dkms remove -m ms912x -v 0.1 --all 2>/dev/null || true
dkms add -m ms912x -v 0.1
dkms build -m ms912x -v 0.1
dkms install -m ms912x -v 0.1 --force

echo "==> [4/4] Carregando o driver ms912x..."
modprobe drm_shmem_helper 2>/dev/null || true
modprobe ms912x

echo ""
echo "Driver instalado e carregado com sucesso!"
lsmod | grep ms912x
