"""Port and persistent-configuration defaults for numbered launcher slots."""
import configparser
from pathlib import Path

MAX_SIMULATORS = 4


def defaults(instance):
    if not 1 <= instance <= MAX_SIMULATORS:
        raise ValueError(f'Simulator number must be between 1 and {MAX_SIMULATORS}')
    offset = instance - 1
    return {'tcp_port': 16001 + 10 * offset, 'udp_port': 16001 + 10 * offset,
            'camera_component_id': 100 + offset}


def mavlink_settings(build, instance, reset=False):
    values = defaults(instance)
    config = Path(build) / 'runtime/app/camera.ini'
    if config.exists() and not reset:
        parser = configparser.ConfigParser(strict=False, interpolation=None)
        try:
            parser.read(config)
        except configparser.Error as error:
            raise ValueError(f'{config}: {error}') from error
        for key in values:
            if parser.has_option('mavlink', key):
                values[key] = int(parser.get('mavlink', key).strip('"\''))
    return values


def initial_config(text, instance):
    """Replace template defaults without introducing duplicate configuration keys."""
    values = defaults(instance)
    section = ''
    lines = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith('[') and stripped.endswith(']'):
            section = stripped[1:-1]
        key = stripped.split('=', 1)[0].strip()
        if section == 'mavlink' and '=' in stripped and key in values:
            line = f'{key} = {values.pop(key)}'
        lines.append(line)
    if values:
        lines.extend(('', '[mavlink]'))
        lines.extend(f'{key} = {value}' for key, value in values.items())
    return '\n'.join(lines) + '\n'


def vendor_stride(target):
    """Leave the vendor's fixed reply port available to local UDP clients."""
    return 10 if target.get('vendor_reply_port') else 1


def vendor_ports(target, command_port):
    tcp = target.get('vendor_tcp_port', command_port) if command_port == target['vendor_port'] else command_port
    return int(tcp), command_port
