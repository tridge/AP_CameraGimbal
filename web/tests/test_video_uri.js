const fs = require('fs');
// The page's video URI rule must agree with apcam_video_uri_valid() (include/apcam/network.h).
const src = fs.readFileSync(process.argv[2], 'utf8');
const start = src.indexOf('const videoUri = text => {');
const end = src.indexOf('// Keep in step with apcam_uart_device_override_valid()');
const videoUri = eval('(' + src.slice(start + 'const videoUri = '.length, end).trim().replace(/;$/, '') + ')');
const valid = ["rtsp://10.0.0.5:8554/video1", "rtsps://pi.local/stream", "rtsp://pi-1.example.com",
  "rtsp://192.168.1.2:1/a/b.264", "rtsp://host:65535/", "rtsp://192.168.144.20:8554"];
const invalid = ["", "10.0.0.5", "http://10.0.0.5/video1", "RTSP://10.0.0.5/video1", "rtsp://",
  "rtsp:///video1", "rtsp://:8554/video1", "rtsp://host:/video1", "rtsp://host:0/video1",
  "rtsp://host:65536/video1", "rtsp://host:123456/video1", "rtsp://host:80x/video1",
  "rtsp://user:pass@host/video1", "rtsp://host?x=1", "rtsp://host/has space",
  'rtsp://host/"quoted"', "rtsp://host/café", "rtsp://[::1]:8554/video1", "rtsp://192.168.144.20:blah"];
const longest = 'rtsp://h/' + 'a'.repeat(159 - 9), tooLong = longest + 'a';
let bad = 0;
for (const u of valid.concat([longest])) if (!videoUri(u)) { console.log('SHOULD BE VALID:', u); bad++; }
for (const u of invalid.concat([tooLong])) if (videoUri(u)) { console.log('SHOULD BE INVALID:', JSON.stringify(u)); bad++; }
// The gimbal link rule must agree with apcam_uart_device_override_valid().
const ipv4 = text => { const p = text.split('.'); return p.length === 4 && p.every(x => /^(0|[1-9][0-9]{0,2})$/.test(x) && Number(x) <= 255); };
const gStart = src.indexOf('const uartDeviceOverride = text =>');
const uartDeviceOverride = new Function('ipv4', 'return (' + src.slice(gStart + 'const uartDeviceOverride = '.length, src.indexOf('const number = text =>', gStart)).trim().replace(/;$/, '') + ')')(ipv4);
const linkValid = ["", "/dev/ttyAMA0", "/dev/serial0", "/dev/serial/by-id/usb-x_y-if00", "udp://192.168.144.15:15002", "udp://localhost:1", "udp://10.0.0.1:65535", "tcp://192.168.144.15:15002", "tcp://localhost:5760"];
const linkInvalid = ["/dev/", "/dev/../etc/passwd", "/etc/passwd", "ttyAMA0", "/dev/tty AMA0", "udp://", "udp://192.168.144.15", "udp://192.168.144.15:", "udp://192.168.144.15:0", "udp://192.168.144.15:65536", "udp://192.168.144.15:123456", "udp://example.com:15002", "udp://300.1.1.1:5", "tcp://192.168.144.15", "tcp://example.com:15002", "tcp://192.168.144.15:80x", "http://192.168.144.15:80", "udp:/192.168.144.15:5"];
for (const u of linkValid) if (!uartDeviceOverride(u)) { console.log('LINK SHOULD BE VALID:', u); bad++; }
for (const u of linkInvalid.concat(['/dev/' + 'a'.repeat(155)])) if (uartDeviceOverride(u)) { console.log('LINK SHOULD BE INVALID:', JSON.stringify(u)); bad++; }
console.log(bad ? 'FAIL video URI page rule: ' + bad + ' mismatches' : 'PASS video URI page rule matches the C validator (' + (valid.length + invalid.length + 2) + ' cases)');
process.exit(bad ? 1 : 0);
