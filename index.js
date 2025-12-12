require('dotenv').config();
const { server, db, JWT_SECRET } = require('./server');
const WebSocket = require('ws');
const jwt = require('jsonwebtoken');

const PORT = process.env.PORT || 3000;

// Crear servidor WebSocket
const wss = new WebSocket.Server({ 
  server,
  path: '/ws'
});

// Almacenar clientes conectados
const clients = {
  devices: new Map(),
  webClients: new Set()
};

// Últimos datos recibidos
let lastData = {
  temperatura: null,
  pulso: null,
  spo2: null,
  timestamp: null
};

// Usuario activo (se establece cuando un cliente web se conecta con token)
let activeUserId = null;

console.log('🚀 Iniciando servidor WebSocket...');

// Broadcast a todos los clientes web
function broadcastToWeb(data) {
  const message = JSON.stringify(data);
  clients.webClients.forEach(client => {
    if (client.readyState === WebSocket.OPEN) {
      client.send(message);
    }
  });
}

// 🆕 FUNCIÓN: Guardar lectura en base de datos
function guardarLecturaEnBD(temperatura, pulso, spo2, deviceId) {
  // Si no hay usuario activo, no guardar
  if (!activeUserId) {
    console.log('⚠️  No hay usuario activo - lectura NO guardada');
    return;
  }

  // Solo guardar si al menos un valor es válido
  if (!temperatura && !pulso && !spo2) {
    console.log('⚠️  Todos los valores son null - lectura NO guardada');
    return;
  }

  const query = `
    INSERT INTO readings (user_id, heart_rate, spo2, temperature, device_id) 
    VALUES (?, ?, ?, ?, ?)
  `;

  db.query(
    query, 
    [activeUserId, pulso, spo2, temperatura, deviceId],
    (err, result) => {
      if (err) {
        console.error('❌ Error al guardar lectura en BD:', err);
        return;
      }
      console.log(`✅ Lectura guardada en BD (ID: ${result.insertId}) - User: ${activeUserId}`);
    }
  );
}

// Manejo de conexiones WebSocket
wss.on('connection', (ws, req) => {
  console.log('📱 Nueva conexión WebSocket desde:', req.socket.remoteAddress);
  
  let clientType = null;
  let deviceType = null;
  let deviceId = null;

  ws.on('message', (message) => {
    try {
      const data = JSON.parse(message);
      console.log('📨 Mensaje recibido:', data);

      // ========== IDENTIFICACIÓN DE DISPOSITIVO ==========
      if (data.type === 'device') {
        clientType = 'device';
        deviceType = data.deviceType;
        deviceId = data.deviceId || 'unknown';
        
        clients.devices.set(deviceId, {
          ws: ws,
          type: deviceType,
          id: deviceId
        });
        
        console.log(`✅ Dispositivo ${deviceType} conectado: ${deviceId}`);
        
        // Notificar a clientes web
        broadcastToWeb({
          type: 'device-status',
          device: deviceType,
          status: 'connected',
          deviceId: deviceId
        });
      }
      
      // ========== IDENTIFICACIÓN DE CLIENTE WEB ==========
      else if (data.type === 'web-client') {
        clientType = 'web-client';
        
        // 🆕 EXTRAER TOKEN DEL CLIENTE WEB
        if (data.token) {
          try {
            const decoded = jwt.verify(data.token, JWT_SECRET);
            activeUserId = decoded.id;
            console.log(`💻 Cliente web conectado - Usuario ID: ${activeUserId} (${decoded.username})`);
          } catch (err) {
            console.error('❌ Token inválido en cliente web:', err.message);
            activeUserId = null;
          }
        } else {
          console.log('💻 Cliente web conectado (sin token)');
        }
        
        clients.webClients.add(ws);
        
        // Enviar estado actual de dispositivos
        const devicesStatus = {
          type: 'devices-status',
          temperatura: Array.from(clients.devices.values()).some(d => d.type === 'temperatura') ? 'connected' : 'disconnected',
          pulso: Array.from(clients.devices.values()).some(d => d.type === 'pulso') ? 'connected' : 'disconnected'
        };
        ws.send(JSON.stringify(devicesStatus));
        
        // Enviar últimos datos si existen
        if (lastData.pulso !== null || lastData.temperatura !== null) {
          ws.send(JSON.stringify({
            type: 'sensor-data',
            ...lastData
          }));
        }
      }
      
      // ========== DATOS DE SENSORES ==========
      else if (data.type === 'sensor-data') {
        // Actualizar últimos datos
        let hasNewData = false;
        
        if (data.temperatura !== undefined && data.temperatura !== null) {
          lastData.temperatura = data.temperatura;
          hasNewData = true;
        }
        if (data.pulso !== undefined && data.pulso !== null) {
          lastData.pulso = data.pulso;
          hasNewData = true;
        }
        if (data.spo2 !== undefined && data.spo2 !== null) {
          lastData.spo2 = data.spo2;
          hasNewData = true;
        }
        lastData.timestamp = Date.now();

        console.log('📊 Datos actualizados:', {
          temp: lastData.temperatura,
          pulso: lastData.pulso,
          spo2: lastData.spo2
        });

        // 🆕 GUARDAR EN BASE DE DATOS
        if (hasNewData) {
          guardarLecturaEnBD(
            lastData.temperatura,
            lastData.pulso,
            lastData.spo2,
            data.deviceId || deviceId || 'unknown'
          );
        }

        // Broadcast a todos los clientes web
        broadcastToWeb({
          type: 'sensor-data',
          temperatura: lastData.temperatura,
          pulso: lastData.pulso,
          spo2: lastData.spo2,
          timestamp: lastData.timestamp
        });
      }
    } catch (error) {
      console.error('❌ Error al procesar mensaje:', error);
    }
  });

  ws.on('close', () => {
    console.log('📴 Conexión cerrada');
    
    if (clientType === 'device' && deviceId) {
      clients.devices.delete(deviceId);
      console.log(`❌ Dispositivo ${deviceType} desconectado: ${deviceId}`);
      
      // Limpiar datos según dispositivo
      if (deviceType === 'pulso') {
        lastData.pulso = null;
        lastData.spo2 = null;
      } else if (deviceType === 'temperatura') {
        lastData.temperatura = null;
      }
      
      // Notificar a clientes web
      broadcastToWeb({
        type: 'device-status',
        device: deviceType,
        status: 'disconnected'
      });
    } else if (clientType === 'web-client') {
      clients.webClients.delete(ws);
      console.log('💻 Cliente web desconectado');
      
      // Si era el último cliente web, limpiar usuario activo
      if (clients.webClients.size === 0) {
        console.log('🔒 No hay clientes web - usuario activo limpiado');
        activeUserId = null;
      }
    }
  });

  ws.on('error', (error) => {
    console.error('❌ Error WebSocket:', error);
  });

  // Ping para mantener conexión
  ws.isAlive = true;
  ws.on('pong', () => {
    ws.isAlive = true;
  });
});

// Verificar conexiones activas cada 30 segundos
setInterval(() => {
  wss.clients.forEach((ws) => {
    if (ws.isAlive === false) {
      return ws.terminate();
    }
    ws.isAlive = false;
    ws.ping();
  });
}, 30000);

// Mostrar estado cada 30 segundos
setInterval(() => {
  console.log('\n📊 ESTADO DEL SISTEMA:');
  console.log('├─ Dispositivos conectados:', clients.devices.size);
  clients.devices.forEach((device, id) => {
    console.log(`│  └─ ${device.type}: ${id}`);
  });
  console.log('├─ Clientes web:', clients.webClients.size);
  console.log('├─ Usuario activo:', activeUserId ? `ID ${activeUserId}` : 'Ninguno');
  console.log('├─ Última temp:', lastData.temperatura !== null ? lastData.temperatura + '°C' : 'N/A');
  console.log('├─ Último pulso:', lastData.pulso !== null ? lastData.pulso + ' BPM' : 'N/A');
  console.log('└─ Último SpO2:', lastData.spo2 !== null ? lastData.spo2 + '%' : 'N/A\n');
}, 30000);

// Iniciar servidor
server.listen(PORT, () => {
  console.log('\n╔════════════════════════════════════════════════════╗');
  console.log('║                                                    ║');
  console.log('║        ✅ SERVIDOR INICIADO CORRECTAMENTE         ║');
  console.log('║                                                    ║');
  console.log('╚════════════════════════════════════════════════════╝');
  console.log(`\n🌐 Servidor HTTP: http://localhost:${PORT}`);
  console.log(`🔌 WebSocket: ws://localhost:${PORT}/ws`);
  console.log(`📊 API REST: http://localhost:${PORT}/api`);
  console.log(`💾 Base de datos: MySQL conectada`);
  console.log('\n💡 Esperando conexiones de dispositivos y clientes web...\n');
});