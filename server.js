require('dotenv').config();
const express = require('express');
const session = require('express-session');
const mysql = require('mysql2');
const jwt = require('jsonwebtoken');
const bcrypt = require('bcryptjs');
const cors = require('cors');
const bodyParser = require('body-parser');
const http = require('http');
const app = express();

const PORT = process.env.PORT;
const JWT_SECRET = process.env.JWT_SECRET;

app.use(cors());
app.use(bodyParser.json());
app.use(bodyParser.urlencoded({ extended: true }));

// Conexión a base de datos
const db = mysql.createConnection({
  host: process.env.DB_HOST,
  user: process.env.DB_USER,
  password: process.env.DB_PASSWORD,
  database: process.env.DB_NAME
});

db.connect((err) => {
  if (err) {
    console.error('Error conectando a la base de datos:', err);
    return;
  }
  console.log('✓ Conectado a la base de datos MySQL');
});

// Middleware JWT
function authenticateToken(req, res, next) {
  const authHeader = req.headers['authorization'];
  const token = authHeader && authHeader.split(' ')[1];
  
  if (!token) {
    return res.status(401).json({ error: 'Token no proporcionado' });
  }
  
  jwt.verify(token, JWT_SECRET, (err, user) => {
    if (err) {
      return res.status(403).json({ error: 'Token inválido' });
    }
    req.user = user;
    next();
  });
}

// ==================== RUTAS API ====================
app.get('/api/test', (req, res) => {
  res.json({ message: 'API funcionando correctamente', timestamp: new Date() });
});

// Registro
app.post('/api/register', async (req, res) => {
  console.log('📝 Solicitud de registro recibida:', req.body);
  
  const { username, email, password, fullName } = req.body;
  
  if (!username || !email || !password) {
    return res.status(400).json({ error: 'Faltan campos requeridos' });
  }
  
  try {
    const hashedPassword = await bcrypt.hash(password, 10);
    
    const query = 'INSERT INTO users (username, email, password, full_name) VALUES (?, ?, ?, ?)';
    db.query(query, [username, email, hashedPassword, fullName], (err, result) => {
      if (err) {
        if (err.code === 'ER_DUP_ENTRY') {
          return res.status(409).json({ error: 'Usuario o email ya existe' });
        }
        return res.status(500).json({ error: 'Error al registrar usuario' });
      }
      res.status(201).json({ message: 'Usuario registrado', userId: result.insertId });
    });
  } catch (error) {
    res.status(500).json({ error: 'Error del servidor' });
  }
});

// Login
app.post('/api/login', (req, res) => {
  const { username, password } = req.body;
  
  if (!username || !password) {
    return res.status(400).json({ error: 'Faltan credenciales' });
  }
  
  const query = 'SELECT * FROM users WHERE username = ?';
  db.query(query, [username], async (err, results) => {
    if (err) return res.status(500).json({ error: 'Error del servidor' });
    if (results.length === 0) return res.status(401).json({ error: 'Credenciales inválidas' });
    
    const user = results[0];
    const validPassword = await bcrypt.compare(password, user.password);
    
    if (!validPassword) return res.status(401).json({ error: 'Credenciales inválidas' });
    
    const token = jwt.sign({ id: user.id, username: user.username }, JWT_SECRET, { expiresIn: '24h' });
    
    res.json({ message: 'Login exitoso', token, user });
  });
});

// Guardar lecturas
app.post('/api/readings', authenticateToken, (req, res) => {
  const { heartRate, spo2, temperature, deviceId } = req.body;
  const userId = req.user.id;

  const query = 'INSERT INTO readings (user_id, heart_rate, spo2, temperature, device_id) VALUES (?, ?, ?, ?, ?)';
  db.query(query, [userId, heartRate, spo2, temperature, deviceId], (err, result) => {
    if (err) return res.status(500).json({ error: 'Error al guardar lectura' });
    res.status(201).json({ message: 'Lectura guardada', readingId: result.insertId });
  });
});

// Obtener lecturas
app.get('/api/readings', authenticateToken, (req, res) => {
  const userId = req.user.id;
  const limit = parseInt(req.query.limit) || 50;
  
  const query = 'SELECT * FROM readings WHERE user_id = ? ORDER BY timestamp DESC LIMIT ?';
  db.query(query, [userId, limit], (err, results) => {
    if (err) return res.status(500).json({ error: 'Error al obtener lecturas' });
    res.json({ readings: results });
  });
});

// Obtener última lectura
app.get('/api/readings/latest', authenticateToken, (req, res) => {
  const userId = req.user.id;
  
  const query = 'SELECT * FROM readings WHERE user_id = ? ORDER BY timestamp DESC LIMIT 1';
  db.query(query, [userId], (err, results) => {
    if (err) return res.status(500).json({ error: 'Error al obtener lectura' });
    res.json({ reading: results[0] || null });
  });
});

// Perfil
app.get('/api/profile', authenticateToken, (req, res) => {
  const userId = req.user.id;
  
  const query = 'SELECT id, username, email, full_name, created_at FROM users WHERE id = ?';
  db.query(query, [userId], (err, results) => {
    if (err || results.length === 0) return res.status(404).json({ error: 'Usuario no encontrado' });
    res.json({ user: results[0] });
  });
});

// Archivos estáticos
app.use(express.static('public'));

// Redirigir al login
app.get('/', (req, res) => {
  res.redirect('/login.html');
});

// ==================== CREAR SERVIDOR HTTP ====================
const server = http.createServer(app);

// Exportar server y db para usarlos en index.js
module.exports = { server, db, JWT_SECRET };

// Solo iniciar el servidor si este archivo se ejecuta directamente
if (require.main === module) {
  server.listen(PORT, () => {
    console.log(`✓ Servidor HTTP corriendo en http://localhost:${PORT}`);
    console.log('⚠️  ADVERTENCIA: Socket.IO debe iniciarse desde index.js');
  });
}