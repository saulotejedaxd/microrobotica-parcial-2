"""Panel local para motor_perfiles.ino. Ejecutar con Python 3 y pyserial."""
import csv
import math
import time
import tkinter as tk
from collections import deque
from tkinter import ttk, messagebox, filedialog

from perfiles import PERFILES, defaults, validate, bounded_value, Telemetry

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    serial = None
    list_ports = None

BLUE, ORANGE, GREEN = '#2563eb', '#d97706', '#059669'


def configure_dpi():
    # Evitar recortes de ventana con el escalado de pantalla de Windows.
    try:
        import ctypes
        ctypes.windll.user32.SetProcessDPIAware()
    except (AttributeError, OSError):
        pass


class Panel:
    def __init__(self, root):
        self.root = root
        root.title('Motor Lab · Arduino UNO · 880 cuentas/vuelta')
        width = min(1200, root.winfo_screenwidth()-50)
        height = min(850, root.winfo_screenheight()-100)
        root.geometry(f'{width}x{height}+20+30')
        root.minsize(min(1020, width), min(640, height))
        self.port = None
        self.ready = False
        self.rx = b''
        self.active = self.ack_run = self.finishing = False
        self.connected_at = self.last_rx = self.last_hello = 0.0
        self.last_send = self.last_ping = self.last_plot = 0.0
        self.started = self.run_sent = 0.0
        self.current = None
        self.data = deque(maxlen=36000)  # Una hora a 10 Hz.
        self.live = deque(maxlen=250)
        self.port_name = tk.StringVar()
        self.profile = tk.StringVar(value='Constante')
        self.status = tk.StringVar(value='Sin conexión · puedes preparar y previsualizar perfiles')
        self.readout = tk.StringVar(value='Real: — RPM     Referencia: — RPM     Posición: —°')
        self.details = tk.StringVar(value='Estado: detenido · HOME: sin verificar')
        self.acc = tk.StringVar(value='120')
        self.dec = tk.StringVar(value='120')
        self.limit = tk.StringVar(value='120')
        self.duration = tk.StringVar(value='20')
        self.params = {}
        style = ttk.Style()
        style.theme_use('clam')
        style.configure('TLabel', font=('Segoe UI', 10))
        style.configure('TButton', font=('Segoe UI', 10), padding=6)
        style.configure('Title.TLabel', font=('Segoe UI', 19, 'bold'))
        style.configure('Read.TLabel', font=('Segoe UI', 13, 'bold'))
        outer = ttk.Frame(root, padding=14)
        outer.pack(fill='both', expand=True)
        ttk.Label(outer, text='Motor Lab', style='Title.TLabel').pack(anchor='w')
        ttk.Label(outer, text='Perfiles de velocidad · control físico · gráficas en vivo').pack(anchor='w')
        connection = ttk.Frame(outer)
        connection.pack(fill='x', pady=8)
        ttk.Label(connection, text='Puerto USB:').pack(side='left')
        self.port_box = ttk.Combobox(connection, textvariable=self.port_name, width=15, state='readonly')
        self.port_box.pack(side='left', padx=5)
        ttk.Button(connection, text='Buscar', command=self.refresh_ports).pack(side='left')
        self.connect_button = ttk.Button(connection, text='Conectar', command=self.connect)
        self.connect_button.pack(side='left', padx=5)
        ttk.Button(connection, text='Desconectar', command=self.disconnect).pack(side='left')
        tk.Button(connection, text='PARAR MOTOR · Esc', bg='#b91c1c', fg='white',
                  font=('Segoe UI', 11, 'bold'), relief='flat', command=self.stop).pack(side='right')
        ttk.Label(outer, textvariable=self.status, wraplength=1130).pack(anchor='w', pady=(0, 8))
        body = ttk.Frame(outer)
        body.pack(fill='both', expand=True)
        left_container = ttk.Frame(body, width=375)
        left_container.pack(side='left', fill='y', padx=(0, 14))
        left_container.pack_propagate(False)
        left_canvas = tk.Canvas(left_container, width=355, highlightthickness=0)
        left_scroll = ttk.Scrollbar(left_container, orient='vertical', command=left_canvas.yview)
        left_scroll.pack(side='right', fill='y')
        left_canvas.pack(side='left', fill='both', expand=True)
        left_canvas.configure(yscrollcommand=left_scroll.set)
        left = ttk.Frame(left_canvas, width=350)
        left_window = left_canvas.create_window((0, 0), window=left, anchor='nw')
        left.bind('<Configure>', lambda _: left_canvas.configure(scrollregion=left_canvas.bbox('all')))
        left_canvas.bind('<Configure>', lambda e: left_canvas.itemconfigure(left_window, width=e.width))
        self.left_canvas = left_canvas
        right = ttk.Frame(body)
        right.pack(side='left', fill='both', expand=True)

        ttk.Label(left, text='PERFIL DESDE PC', font=('Segoe UI', 11, 'bold')).pack(anchor='w')
        box = ttk.Combobox(left, textvariable=self.profile, values=list(PERFILES), state='readonly')
        box.pack(fill='x', pady=5)
        box.bind('<<ComboboxSelected>>', self.profile_changed)
        self.explanation = ttk.Label(left, wraplength=345)
        self.explanation.pack(fill='x', pady=(0, 5))
        self.param_frame = ttk.Frame(left)
        self.param_frame.pack(fill='x')
        common = ttk.LabelFrame(left, text='Límites y duración', padding=7)
        common.pack(fill='x', pady=7)
        for row, (label, var) in enumerate([
            ('Aceleración máxima (RPM/s)', self.acc),
            ('Desaceleración máxima (RPM/s)', self.dec),
            ('Límite de velocidad |RPM|', self.limit),
            ('Duración de la prueba (s)', self.duration),
        ]):
            ttk.Label(common, text=label).grid(row=row, column=0, sticky='w', pady=2)
            ttk.Entry(common, textvariable=var, width=9).grid(row=row, column=1, padx=6)
        ttk.Button(left, text='Previsualizar cambios', command=self.preview).pack(fill='x', pady=2)
        ttk.Button(left, text='INICIAR perfil desde PC', command=self.start).pack(fill='x', pady=2)
        ttk.Button(left, text='Aplicar en vivo / reiniciar curva', command=self.apply_live).pack(fill='x', pady=2)
        tk.Button(left, text='PARAR MOTOR  ·  Esc', bg='#b91c1c', fg='white',
                  font=('Segoe UI', 12, 'bold'), relief='flat', command=self.stop).pack(fill='x', pady=7)
        local = ttk.LabelFrame(left, text='Control físico y HOME', padding=5)
        local.pack(fill='x')
        ttk.Button(local, text='Volver al control físico', command=self.local).pack(fill='x', pady=2)
        row = ttk.Frame(local)
        row.pack(fill='x')
        ttk.Button(row, text='Guardar cero', command=lambda: self.action('ZERO')).pack(side='left', expand=True, fill='x')
        ttk.Button(row, text='Ir a HOME', command=self.home).pack(side='left', expand=True, fill='x')
        ttk.Button(local, text='Borrar fallo (no arranca)', command=lambda: self.action('CLEAR')).pack(fill='x', pady=2)
        ttk.Label(left, text='D7 siempre permite parar. D4/D10 cancelan el perfil PC.\n'
                  'En físico: potenciómetro y botones funcionan como antes.',
                  wraplength=345, font=('Segoe UI', 9)).pack(anchor='w', pady=7)

        ttk.Label(right, textvariable=self.readout, style='Read.TLabel', wraplength=790).pack(anchor='w')
        ttk.Label(right, textvariable=self.details).pack(anchor='w', pady=(3, 5))
        ttk.Label(right, text='EN VIVO · últimos 20 segundos · RPM').pack(anchor='w')
        self.live_canvas = tk.Canvas(right, bg='white', highlightthickness=1, highlightbackground='#cbd5e1', height=250)
        self.live_canvas.pack(fill='both', expand=True)
        legend = ttk.Frame(right)
        legend.pack(fill='x', pady=4)
        for text, color in [('Solicitada', BLUE), ('Limitada por aceleración', ORANGE), ('Medida por encoder', GREEN)]:
            ttk.Label(legend, text='━ ' + text, foreground=color).pack(side='left', padx=(0, 10))
        ttk.Label(right, text='VISTA PREVIA · perfil solicitado, recortado al límite de RPM').pack(anchor='w', pady=(5, 3))
        self.preview_canvas = tk.Canvas(right, bg='white', height=140, highlightthickness=1, highlightbackground='#cbd5e1')
        self.preview_canvas.pack(fill='x')
        bottom = ttk.Frame(right)
        bottom.pack(fill='x', pady=5)
        ttk.Button(bottom, text='Guardar mediciones CSV', command=self.export).pack(side='left')
        ttk.Label(bottom, text='RPM negativas = giro contrario').pack(side='right')
        self.log_box = tk.Text(right, height=4, font=('Consolas', 9), state='disabled', wrap='word')
        self.log_box.pack(fill='x')
        root.bind('<Escape>', lambda _: self.stop())
        root.bind('<MouseWheel>', lambda e: left_canvas.yview_scroll(-int(e.delta/120), 'units')
                  if e.x_root < left_container.winfo_rootx()+left_container.winfo_width() else None)
        root.protocol('WM_DELETE_WINDOW', self.close)
        self.profile_changed()
        self.refresh_ports()
        root.after(100, self.preview)
        root.after(20, self.tick)

    def log(self, text):
        self.log_box.configure(state='normal')
        self.log_box.insert('end', time.strftime('%H:%M:%S ') + text + '\n')
        if int(self.log_box.index('end-1c').split('.')[0]) > 150:
            self.log_box.delete('1.0', '30.0')
        self.log_box.see('end')
        self.log_box.configure(state='disabled')

    def refresh_ports(self):
        ports = [p.device for p in list_ports.comports()] if list_ports else []
        self.port_box['values'] = ports
        if ports and self.port_name.get() not in ports:
            self.port_name.set(ports[0])

    def profile_changed(self, _=None):
        for w in self.param_frame.winfo_children():
            w.destroy()
        self.params = {}
        fields, text = PERFILES[self.profile.get()]
        self.explanation.configure(text=text)
        for i, (key, label, default, _, _) in enumerate(fields):
            var = tk.StringVar(value=str(default))
            self.params[key] = var
            ttk.Label(self.param_frame, text=label).grid(row=i, column=0, sticky='w', pady=2)
            ttk.Entry(self.param_frame, textvariable=var, width=10).grid(row=i, column=1, padx=6)
        if self.active:
            self.status.set('Cambios preparados. Pulsa «Aplicar en vivo» para enviarlos.')

    def config(self):
        name = self.profile.get()
        params = validate(name, {k: v.get() for k, v in self.params.items()})
        common = []
        for var, label, lo, hi in [(self.acc, 'Aceleración', 1, 1000),
                                   (self.dec, 'Desaceleración', 1, 1000),
                                   (self.limit, 'Límite RPM', 1, 280),
                                   (self.duration, 'Duración', 1, 3600)]:
            try:
                n = float(var.get().replace(',', '.'))
            except ValueError:
                raise ValueError(f'{label}: escribe un número.') from None
            if not math.isfinite(n) or not lo <= n <= hi:
                raise ValueError(f'{label}: rango {lo} a {hi}.')
            common.append(n)
        return (name, params, *common)

    def get_config(self):
        try:
            return self.config()
        except ValueError as e:
            messagebox.showerror('Revisa los parámetros', str(e))
            return None

    def preview(self):
        c = self.get_config()
        if c:
            name, p, acc, dec, limit, duration = c
            points = [(i*duration/400, bounded_value(name, p, i*duration/400, limit)) for i in range(401)]
            self.chart(self.preview_canvas, [(points, BLUE)], 0, duration)

    def connect(self):
        if self.port:
            return
        if not serial:
            messagebox.showerror('Falta pyserial', 'Ejecuta instalar_y_abrir.bat para instalar pyserial.')
            return
        if not self.port_name.get():
            messagebox.showinfo('Puerto', 'Conecta Arduino por USB y pulsa Buscar.')
            return
        try:
            self.port = serial.Serial(self.port_name.get(), 115200, timeout=0, write_timeout=0.1)
        except (serial.SerialException, OSError) as e:
            messagebox.showerror('No se pudo conectar', f'{e}\nCierra el Monitor Serie de Arduino.')
            return
        self.rx = b''
        self.ready = False
        self.active = self.ack_run = False
        self.connected_at = self.last_rx = time.monotonic()
        self.last_hello = 0
        self.live.clear()
        self.status.set('Conectado al puerto; esperando firmware MOTOR9…')
        self.log('Abrir USB puede reiniciar UNO: vuelve a guardar HOME si se perdió.')

    def send(self, command):
        if not self.port:
            return False
        try:
            payload = (command + '\n').encode('ascii')
            if self.port.write(payload) != len(payload):
                raise OSError('Envío incompleto')
            return True
        except (OSError, serial.SerialException) as e:
            self.disconnect(send_stop=False)
            self.status.set('Enlace perdido. Arduino detiene el control PC por timeout.')
            self.log(str(e))
            return False

    def require_link(self):
        if self.port and self.ready:
            return True
        messagebox.showinfo('Conecta Arduino', 'Conecta el puerto y espera a que el firmware quede listo.')
        return False

    def setpoint(self, target):
        _, _, acc, dec, _, _ = self.current
        return self.send(f'SET,{target:.3f},{acc:.3f},{dec:.3f}')

    def start(self):
        if not self.require_link():
            return
        c = self.get_config()
        if not c:
            return
        if self.active:
            self.apply_live()
            return
        self.current = c
        self.started = self.run_sent = time.monotonic()
        self.finishing = self.ack_run = False
        name, p, _, _, limit, _ = c
        if (self.send('MODE,PC') and self.setpoint(bounded_value(name, p, 0, limit)) and self.send('RUN')):
            self.active = True
            self.status.set('Esperando confirmación de arranque…')
            self.log('Inicio solicitado: ' + name)

    def apply_live(self):
        if not self.active:
            messagebox.showinfo('Perfil detenido', 'Pulsa INICIAR para comenzar la prueba.')
            return
        c = self.get_config()
        if c:
            self.current = c
            self.started = time.monotonic()
            self.finishing = False
            self.log('Aplicado en vivo: ' + c[0] + '. Tiempo de curva reiniciado; rampa de velocidad conservada.')
            self.preview()

    def stop(self):
        self.active = self.ack_run = self.finishing = False
        if self.port:
            self.send('STOP')
        self.status.set('Paro solicitado · corte de potencia, sin rampa')

    def local(self):
        if not self.require_link():
            return
        self.active = self.ack_run = False
        self.send('MODE,LOCAL')
        self.status.set('Control físico solicitado · arranca con D7; gráficas siguen activas')

    def home(self):
        if not self.require_link():
            return
        self.active = self.ack_run = False
        if self.send('STOP'):
            self.send('HOME')
        self.status.set('HOME solicitado · requiere cero previamente guardado')

    def action(self, cmd):
        if self.require_link():
            if cmd == 'CLEAR':
                self.active = self.ack_run = False
            self.send(cmd)

    def disconnect(self, send_stop=True):
        self.active = self.ack_run = self.ready = False
        port, self.port = self.port, None
        if port:
            try:
                if send_stop:
                    port.write(b'STOP\n')
                port.close()
            except Exception:
                try:
                    port.close()
                except Exception:
                    pass
        self.status.set('Desconectado · control físico disponible en Arduino')

    def on_line(self, line, now):
        if line.startswith('HELLO,MOTOR9,1,'):
            if self.ready:
                # Un reinicio detectado jamás reanuda el perfil.
                self.active = self.ack_run = False
                self.log('Arduino reiniciado; perfil cancelado. Revisa HOME.')
            self.ready = True
            self.last_rx = now
            self.status.set('Arduino listo · control físico disponible · firmware MOTOR9')
            self.send('STOP')
            return
        if line == 'OK,RUN':
            self.ack_run = self.active
            self.status.set('Perfil PC ejecutándose · puedes aplicar cambios en vivo')
        elif line.startswith('OK,'):
            self.log(line)
        elif line.startswith('ERR,') or line.startswith('FALLO:'):
            self.log(line)
            explanations = {
                'ERR,MODE': 'El control físico tomó el mando; perfil PC cancelado',
                'ERR,START': 'Arranque rechazado: revisa modo PC, comunicación y fallos',
                'ERR,STOP_FIRST': 'Detén el motor y espera un momento antes de guardar cero',
                'ERR,NO_HOME': 'Primero guarda el cero; si hay un fallo, bórralo antes',
                'ERR,PARAM': 'Parámetros rechazados por Arduino',
            }
            self.status.set(explanations.get(line, 'Arduino: ' + line))
            if self.active:
                self.active = self.ack_run = False
                # Una orden SET que quedó en tránsito tras D10/D4 no debe
                # cancelar el HOME o la acción física con un STOP tardío.
                if line not in ('ERR,MODE', 'ERR,START'):
                    self.send('STOP')
        elif line.startswith('T,'):
            try:
                t = Telemetry.parse(line)
            except ValueError:
                return
            self.last_rx = now
            self.data.append((now, t))
            self.live.append((now, t))
            names = ['Físico POS', 'Físico VEL', 'PC', 'HOME']
            states = ['Detenido', 'Activo', 'Volviendo a cero']
            self.readout.set(f'Real: {t.rpm:.1f} RPM    Referencia: {t.limited:.1f} RPM    Posición: {t.angle:.1f}°')
            self.details.set(f'{names[t.mode]} · {states[t.state]} · HOME: {"guardado" if t.home else "sin definir"}'
                             f' · PWM: {t.pwm}' + (' · FALLO' if t.fault else ''))
            if self.active and self.ack_run:
                if t.mode != 2 or t.state != 1 or t.fault:
                    self.active = self.ack_run = False
                    self.status.set('Perfil cancelado por botón físico, cambio de modo o fallo')
                elif self.finishing and abs(t.limited) < 0.05:
                    self.stop()
                    self.status.set('Prueba terminada · referencia desacelerada a cero')

    def tick(self):
        now = time.monotonic()
        if self.port:
            try:
                self.rx += self.port.read(min(self.port.in_waiting, 8192))
                if len(self.rx) > 16384:
                    raise OSError('Se perdió la sincronización serial')
                while b'\n' in self.rx:
                    line, self.rx = self.rx.split(b'\n', 1)
                    self.on_line(line.decode('ascii', errors='replace').strip(), now)
            except (OSError, serial.SerialException) as e:
                self.disconnect(send_stop=False)
                self.log('Enlace perdido: ' + str(e))
            if self.port and not self.ready:
                if now - self.last_hello > 1:
                    self.send('HELLO?')
                    self.last_hello = now
                if now - self.connected_at > 8:
                    self.disconnect()
                    self.status.set('No se detectó MOTOR9. Carga motor_perfiles.ino y vuelve a conectar.')
            elif self.port:
                if now - self.last_rx > 2:
                    self.disconnect()
                    self.status.set('Sin telemetría: desconectado y paro solicitado')
                elif now - self.last_ping > 0.25:
                    self.send('PING')
                    self.last_ping = now
        if self.active and self.port and self.ready:
            if not self.ack_run and now - self.run_sent > 1.5:
                self.stop()
                self.status.set('No se confirmó el arranque. Revisa el registro.')
            elif now - self.last_send >= 0.05:
                name, p, _, _, limit, duration = self.current
                elapsed = now - self.started
                if elapsed >= duration:
                    self.finishing = True
                target = 0 if self.finishing else bounded_value(name, p, elapsed, limit)
                self.setpoint(target)
                self.last_send = now
                if self.finishing:
                    self.status.set('Duración cumplida · desacelerando hacia cero…')
        if now - self.last_plot >= 0.1:
            series = []
            for attr, color in [('requested', BLUE), ('limited', ORANGE), ('rpm', GREEN)]:
                series.append(([(stamp-now, getattr(t, attr)) for stamp, t in self.live if stamp >= now-20], color))
            self.chart(self.live_canvas, series, -20, 0)
            self.last_plot = now
        self.root.after(20, self.tick)

    @staticmethod
    def chart(canvas, series, xmin, xmax):
        canvas.delete('all')
        w, h = canvas.winfo_width(), canvas.winfo_height()
        if w < 100 or h < 80:
            return
        left, top, right, bottom = 52, 12, w-15, h-27
        values = [v for points, _ in series for _, v in points]
        lo, hi = min([0, *values]), max([10, *values])
        pad = max(5, (hi-lo)*0.1)
        lo, hi = lo-pad, hi+pad
        def xy(x, y):
            return (left+(x-xmin)/(xmax-xmin)*(right-left), bottom-(y-lo)/(hi-lo)*(bottom-top))
        for i in range(5):
            y = lo + (hi-lo)*i/4
            _, py = xy(xmin, y)
            canvas.create_line(left, py, right, py, fill='#e2e8f0')
            canvas.create_text(left-5, py, text=f'{y:.0f}', anchor='e', fill='#475569')
        for i in range(6):
            x = xmin + (xmax-xmin)*i/5
            px, _ = xy(x, lo)
            canvas.create_line(px, top, px, bottom, fill='#f1f5f9')
            canvas.create_text(px, bottom+13, text=f'{x:.1f}s', fill='#475569')
        for points, color in series:
            coords = [c for x, y in points for c in xy(x, y)]
            if len(coords) >= 4:
                canvas.create_line(*coords, fill=color, width=2)

    def export(self):
        if not self.data:
            messagebox.showinfo('Sin mediciones', 'Primero conecta Arduino para recibir datos reales.')
            return
        name = filedialog.asksaveasfilename(defaultextension='.csv', initialfile='mediciones_motor.csv', filetypes=[('CSV', '*.csv')])
        if name:
            try:
                with open(name, 'w', newline='', encoding='utf-8-sig') as f:
                    out = csv.writer(f)
                    out.writerow(['pc_s', 'arduino_ms', 'modo', 'estado', 'home', 'fallo', 'rpm_solicitada', 'rpm_limitada', 'rpm_real', 'grados', 'pwm'])
                    first = self.data[0][0]
                    for stamp, t in self.data:
                        out.writerow([round(stamp-first, 3), t.ms, t.mode, t.state, int(t.home), int(t.fault), t.requested, t.limited, t.rpm, t.angle, t.pwm])
                self.log('CSV guardado: ' + name)
            except OSError as e:
                messagebox.showerror('No se pudo guardar', str(e))

    def close(self):
        self.disconnect()
        self.root.destroy()


if __name__ == '__main__':
    configure_dpi()
    window = tk.Tk()
    Panel(window)
    window.mainloop()
