"""Local protocol fixture; not the competition service or a model implementation."""
import json
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

LOG = Path(os.environ.get('AI_MOCK_LOG', '/tmp/seekdb-ai-mock-requests.jsonl'))
LOCK = threading.Lock()

class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        try:
            size = int(self.headers.get('Content-Length', '0'))
            if size > 8 * 1024 * 1024:
                raise ValueError('request too large')
            body = json.loads(self.rfile.read(size))
            with LOCK, LOG.open('a') as out:
                out.write(json.dumps({'path': self.path, 'body': body}, ensure_ascii=False) + '\n')
            model = body.get('model', '')
            if model == 'fixture-http-error':
                self.send_json(503, {'error': 'intentional local test failure'})
                return
            if self.path.endswith('/chat/completions'):
                content = body['messages'][-1]['content']
                if model == 'fixture-empty':
                    content = ''
                self.send_json(200, {'choices': [{'message': {'content': content}}]})
            elif self.path.endswith('/generate'):
                if body.get('stream') is not False:
                    raise ValueError('native generation must disable streaming')
                self.send_json(200, {'response': body['prompt'], 'done': True})
            elif self.path.endswith('/embeddings') or self.path.endswith('/embed'):
                inputs = body['input']
                if isinstance(inputs, str):
                    inputs = [inputs]
                dim = body.get('dimensions', 768 if model == 'text-embedding-v1' else 3)
                vectors = [[float(len(value))] + [float(i) for i in range(1, dim)] for value in inputs]
                if model == 'fixture-invalid-vector':
                    vectors[0][0] = 'invalid'
                if self.path.endswith('/embed'):
                    self.send_json(200, {'embeddings': vectors})
                else:
                    # Deliberately reverse records to test index-based restoration.
                    data = [{'index': i, 'embedding': vector} for i, vector in enumerate(vectors)]
                    if model == 'fixture-duplicate-index' and len(data) > 1:
                        data[-1]['index'] = 0
                    self.send_json(200, {'data': data[::-1]})
            elif self.path.endswith('/rerank'):
                docs = body['documents']
                results = [{'index': i, 'relevance_score': len(doc) % 3 / 2.0} for i, doc in enumerate(docs)]
                if model == 'fixture-invalid-rank':
                    results[0]['index'] = len(docs)
                self.send_json(200, {'results': results[::-1]})
            else:
                self.send_json(404, {'error': 'unknown fixture route'})
        except (KeyError, TypeError, ValueError) as error:
            self.send_json(400, {'error': str(error)})

    def send_json(self, status, body):
        payload = json.dumps(body, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

if __name__ == '__main__':
    ThreadingHTTPServer(('127.0.0.1', 8000), Handler).serve_forever()
