"""Optional PC LAN bridge, forwards bytes without transcoding or changing IPs."""
import argparse,http.client,http.server,urllib.parse
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--rk',default='http://192.168.50.2:8080');parser.add_argument('--listen',default='0.0.0.0');parser.add_argument('--port',type=int,default=8081);a=parser.parse_args();target=urllib.parse.urlsplit(a.rk)
    if target.scheme!='http' or not target.hostname:parser.error('Use http://RK-IP:8080')
    class Relay(http.server.BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'
        def log_message(self,*_):pass
        def forward(self):
            connection=http.client.HTTPConnection(target.hostname,target.port or 80,timeout=7)
            try:
                size=int(self.headers.get('Content-Length','0'))
                if size>32768:self.send_error(413);return
                headers={k:v for k,v in self.headers.items() if k.lower() not in ('host','connection','origin')}
                connection.request(self.command,self.path,body=self.rfile.read(size) if size else None,headers=headers);reply=connection.getresponse()
                self.send_response(reply.status)
                for k,v in reply.getheaders():
                    if k.lower() not in ('connection','transfer-encoding','server','date'):self.send_header(k,v)
                self.send_header('Connection','close');self.end_headers();self.close_connection=True
                while True:
                    chunk=reply.read1(65536)
                    if not chunk:break
                    self.wfile.write(chunk);self.wfile.flush()
            except (OSError,TimeoutError):self.close_connection=True
            finally:connection.close()
        do_GET=forward;do_POST=forward
    print('Phone: http://<PC-WIFI-IP>:'+str(a.port)+'/#token=<same RK token>',flush=True)
    server=http.server.ThreadingHTTPServer((a.listen,a.port),Relay);server.daemon_threads=True
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:server.server_close()
if __name__=='__main__':main()
