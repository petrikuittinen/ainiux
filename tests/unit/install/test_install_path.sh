#!/usr/bin/env bash

set -euo pipefail

repository_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
verifier="${repository_root}/scripts/verify-install-path.sh"
install_test_root=$(mktemp -d)
trap 'rm -rf -- "${install_test_root}"' EXIT HUP INT TERM

write_test_binary() {
    local path="$1"
    local marker="$2"
    mkdir -p -- "$(dirname -- "${path}")"
    printf '#!/usr/bin/env bash\nprintf '\''%%s\\n'\'' '\''%s'\''\n' "${marker}" >"${path}"
    chmod 0755 "${path}"
}

installed="${install_test_root}/system/bin/ainiux"
test_home="${install_test_root}/home"
user_local="${test_home}/.local/bin/ainiux"
write_test_binary "${installed}" new-build
write_test_binary "${user_local}" stale-build

verified_output=$(env AINIUX_INSTALL_USER_HOME="${test_home}" \
    PATH="${test_home}/.local/bin:${install_test_root}/system/bin:/usr/bin:/bin" \
    "${verifier}" "${installed}" "${install_test_root}/system")
cmp -s "${installed}" "${user_local}"
case "${verified_output}" in
    *"Refreshed shadowing user-local executable"*"Verified synchronized PATH duplicate"*) ;;
    *)
        echo "install verifier did not report the refreshed PATH shadow" >&2
        exit 1
        ;;
esac

custom_shadow="${install_test_root}/custom/bin/ainiux"
write_test_binary "${custom_shadow}" unmanaged-stale-build
if env AINIUX_INSTALL_USER_HOME="${install_test_root}/empty-home" \
    PATH="${install_test_root}/custom/bin:${install_test_root}/system/bin:/usr/bin:/bin" \
    "${verifier}" "${installed}" "${install_test_root}/system" \
    >"${install_test_root}/custom.stdout" 2>"${install_test_root}/custom.stderr"; then
    echo "install verifier accepted a mismatched unmanaged PATH shadow" >&2
    exit 1
fi
grep -q "PATH still resolves ainiux to a different executable" \
    "${install_test_root}/custom.stderr"

symlink_home="${install_test_root}/symlink-home"
mkdir -p -- "${symlink_home}/.local/bin"
ln -s "${custom_shadow}" "${symlink_home}/.local/bin/ainiux"
if env AINIUX_INSTALL_USER_HOME="${symlink_home}" \
    PATH="${symlink_home}/.local/bin:${install_test_root}/system/bin:/usr/bin:/bin" \
    "${verifier}" "${installed}" "${install_test_root}/system" \
    >"${install_test_root}/symlink.stdout" 2>"${install_test_root}/symlink.stderr"; then
    echo "install verifier replaced or accepted an unsafe user-local symlink" >&2
    exit 1
fi
grep -q "refused to replace the shadowing symlink" "${install_test_root}/symlink.stderr"
[ -L "${symlink_home}/.local/bin/ainiux" ]

identical_shadow="${install_test_root}/identical/bin/ainiux"
mkdir -p -- "$(dirname -- "${identical_shadow}")"
cp "${installed}" "${identical_shadow}"
chmod 0755 "${identical_shadow}"
env AINIUX_INSTALL_USER_HOME="${install_test_root}/empty-home" \
    PATH="${install_test_root}/identical/bin:${install_test_root}/system/bin:/usr/bin:/bin" \
    "${verifier}" "${installed}" "${install_test_root}/system" \
    >"${install_test_root}/identical.stdout"
grep -q "Verified synchronized PATH duplicate" "${install_test_root}/identical.stdout"

off_path_home="${install_test_root}/off-path-home"
off_path_shadow="${off_path_home}/.local/bin/ainiux"
write_test_binary "${off_path_shadow}" deliberately-separate-build
env AINIUX_INSTALL_USER_HOME="${off_path_home}" \
    PATH="${off_path_home}/.local/bin:/usr/bin:/bin" \
    "${verifier}" "${installed}" "${install_test_root}/system" \
    >"${install_test_root}/off-path.stdout"
grep -q "is not on PATH; executable shadow verification was skipped" \
    "${install_test_root}/off-path.stdout"
grep -q "deliberately-separate-build" "${off_path_shadow}"

fake_command_directory="${install_test_root}/fake-command/bin"
write_test_binary "${fake_command_directory}/id" 0
if env SUDO_USER=test-user \
    PATH="${fake_command_directory}:/usr/bin:/bin" \
    "${repository_root}/scripts/install.sh" --no-deps \
    --prefix "${install_test_root}/sudo-prefix" \
    >"${install_test_root}/sudo.stdout" 2>"${install_test_root}/sudo.stderr"; then
    echo "installer accepted a whole-script sudo invocation" >&2
    exit 1
fi
grep -q "Do not run the whole installer through sudo" \
    "${install_test_root}/sudo.stderr"

echo "install PATH verification tests passed"
