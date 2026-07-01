import os
import gzip
import shutil
import hashlib
import subprocess
from utils import sysenv

Import('env')  # type: ignore


def dir_ensure(dir):
    if not os.path.exists(dir):
        os.makedirs(dir)


def dir_delete(dir):
    if os.path.exists(dir):
        shutil.rmtree(dir)


def file_delete(file):
    if os.path.exists(file):
        os.remove(file)


def file_gzip(file_path, gzip_path):
    size_before = os.path.getsize(file_path)
    dir_ensure(os.path.dirname(gzip_path))
    file_delete(gzip_path)
    with open(file_path, 'rb') as f_in, gzip.open(gzip_path, 'wb') as f_out:
        f_out.write(f_in.read())
    size_after = os.path.getsize(gzip_path)
    print(f'Gzipped {file_path}: {size_before} => {size_after} bytes')


def file_write_bin(file, data):
    try:
        with open(file, 'wb') as f:
            f.write(data)
        return True
    except:
        print('Error writing to ' + file)
        return False


def file_read_text(file):
    try:
        with open(file, 'r', encoding='utf-8') as f:
            return (f.read(), 'utf-8')
    except:
        pass
    try:
        with open(file, 'r') as f:
            return (f.read(), None)
    except:
        print('Error reading ' + file)
        return (None, None)


def file_write_text(file, text, enc):
    try:
        with open(file, 'w', encoding=enc) as f:
            f.write(text)
        return True
    except:
        print('Error writing to ' + file)
        return False


def ensure_data_placeholder():
    dir_ensure('data')
    placeholder = os.path.join('data', '.keep')
    if not os.path.exists(placeholder):
        file_write_text(placeholder, 'placeholder\n', 'utf-8')


def resolve_cmd(*names):
    for name in names:
        exe = shutil.which(name)
        if exe and os.path.exists(exe):
            return exe
    return None


def build_frontend(source, target, env):
    project_root = os.getcwd()
    frontend_dir = os.path.join(project_root, 'frontend')

    # Always ensure data dir exists so littlefs has a valid source directory.
    ensure_data_placeholder()

    # Build frontend unless in CI where artifacts may already be prepared.
    if not sysenv.get_bool('CI', False):
        print('Building frontend...')

        package_manager = None
        package_manager_exe = None
        install_cmd = None
        build_cmd = None

        pnpm_exe = resolve_cmd('pnpm.cmd', 'pnpm')
        corepack_exe = resolve_cmd('corepack.cmd', 'corepack')
        npm_exe = resolve_cmd('npm.cmd', 'npm')

        if pnpm_exe:
            package_manager = 'pnpm'
            package_manager_exe = pnpm_exe
            install_cmd = [pnpm_exe, 'i']
            build_cmd = [pnpm_exe, 'run', 'build']
        elif corepack_exe:
            package_manager = 'corepack pnpm'
            package_manager_exe = corepack_exe
            install_cmd = [corepack_exe, 'pnpm', 'i']
            build_cmd = [corepack_exe, 'pnpm', 'run', 'build']
        elif npm_exe:
            package_manager = 'npm'
            package_manager_exe = npm_exe
            # Local dev machines can have newer Node than pinned engines; allow install fallback.
            install_cmd = [npm_exe, 'install', '--force', '--no-audit', '--no-fund']
            build_cmd = [npm_exe, 'run', 'build']

        if package_manager is None:
            print('No package manager found (pnpm/corepack/npm). Skipping frontend build.')
        else:
            print(f'Using {package_manager} ({package_manager_exe}) to build frontend')
            try:
                install_rc = subprocess.call(install_cmd, cwd=frontend_dir)
                build_rc = subprocess.call(build_cmd, cwd=frontend_dir) if install_rc == 0 else -1
                if install_rc != 0 or build_rc != 0:
                    print('Frontend build failed; using existing frontend/build output if available.')
                else:
                    print('Frontend build complete.')
            except OSError as e:
                print(f'Failed to execute frontend build command: {e}')
                print('Using existing frontend/build output if available.')

    # Shorten all the filenames in the data/www/_app/immutable directory.
    copy_actions = []
    for root, _, files in os.walk('frontend/build'):
        root = root.replace('\\', '/')
        newroot = root.replace('frontend/build', 'data/www')

        for filename in files:
            filepath = os.path.join(root, filename)

            newfilepath = os.path.join(newroot, filename)

            copy_actions.append((filepath, newfilepath))

    if len(copy_actions) == 0:
        if os.path.exists('data/www'):
            print('No frontend build artifacts found; keeping existing data/www content.')
            ensure_data_placeholder()
            return
        raise RuntimeError('No frontend build artifacts found and data/www is missing. Cannot produce LittleFS image with UI assets.')

    # Delete the data/www directory if it exists.
    dir_delete('data/www')

    for src_path, dst_path in copy_actions:
        dir_ensure(os.path.dirname(dst_path))
        if src_path.endswith('.gz'):
            shutil.copyfile(src_path, dst_path)
        else:
            file_gzip(src_path, dst_path + '.gz')


def hash_file(filepath):
    md5 = hashlib.md5()
    sha1 = hashlib.sha1()
    sha256 = hashlib.sha256()

    with open(filepath, 'rb') as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            md5.update(chunk)
            sha1.update(chunk)
            sha256.update(chunk)

    return {
        'MD5': md5.hexdigest(),
        'SHA1': sha1.hexdigest(),
        'SHA256': sha256.hexdigest(),
    }


def process_littlefs_binary(source, target, env):
    nTargets = len(target)
    if nTargets != 1:
        raise Exception(f'Expected 1 target, got {nTargets}')

    # Get the path to the binary and its directory.
    littlefs_path = target[0].get_abspath()

    bin_size = os.path.getsize(littlefs_path)
    bin_hashes = hash_file(littlefs_path)

    print(f'FileSystem Size: {bin_size} bytes')
    print('FileSystem Hashes:')
    print('MD5:    ' + bin_hashes['MD5'])
    print('SHA1:   ' + bin_hashes['SHA1'])
    print('SHA256: ' + bin_hashes['SHA256'])


env.AddPreAction('$BUILD_DIR/littlefs.bin', build_frontend)
env.AddPostAction('$BUILD_DIR/littlefs.bin', process_littlefs_binary)
