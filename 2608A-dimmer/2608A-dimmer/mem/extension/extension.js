const vscode = require('vscode');
const path = require('path');

// Get the current stack frame id (needed for 'watch'/'hover' evaluations).
async function getFrameId(session) {
  // Newer VS Code API: the focused stack item.
  const item = vscode.debug.activeStackItem;
  if (item && typeof item.frameId === 'number') {
    return item.frameId;
  }
  // Fallback: ask the debug adapter directly.
  try {
    let tid = item && typeof item.threadId === 'number' ? item.threadId : undefined;
    if (tid === undefined) {
      const threads = await session.customRequest('threads');
      tid = threads && threads.threads && threads.threads[0] && threads.threads[0].id;
    }
    if (tid === undefined) return undefined;
    const st = await session.customRequest('stackTrace', {
      threadId: tid, startFrame: 0, levels: 1
    });
    return st && st.stackFrames && st.stackFrames[0] && st.stackFrames[0].id;
  } catch (e) {
    return undefined;
  }
}

function activate(context) {
  const cmd = vscode.commands.registerCommand('mem', async () => {
    const editor = vscode.window.activeTextEditor;
    if (!editor) {
      //vscode.window.showWarningMessage('mem: no active editor');
      return;
    }

    const session = vscode.debug.activeDebugSession;
    if (!session) {
      //vscode.window.showWarningMessage('mem: no active debug session');
      return;
    }

    
    // get isPythonProgram, isPythonFile
    isPythonProgram = false;

    const session_type = (session.type || '').toLowerCase();
    if (['debugpy', 'python', 'pythonexperimental'].includes(session_type)) {
        isPythonProgram = true;
    }

    const langId = editor.document.languageId; // e.g. 'python', 'cpp', 'c'
    const ext = path.extname(editor.document.uri.fsPath).toLowerCase(); // e.g. '.py', '.cpp'

    const isPythonFile = langId === 'python' || ext === '.py';


    // get filePath, projectPath, extensionPath
    const filePath = path.dirname(editor.document.uri.fsPath).replace(/\\/g, '/');

    let projectPath = filePath
    const folder = vscode.workspace.getWorkspaceFolder(editor.document.uri);
    if (folder) {
        projectPath = folder.uri.fsPath.replace(/\\/g, '/');
    }

    const extensionPath = __dirname.replace(/\\/g, '/');
    

    // get frameId
    const frameId = await getFrameId(session);
    let expression = null;

    if (!isPythonProgram) {
      if(!isPythonFile){ // c++ program, c++ code 
        let text;
        if (editor.selection.isEmpty) {
          const pos = editor.selection.active;               
          const range = editor.document.getWordRangeAtPosition(pos);
          if (!range){
            //vscode.window.showErrorMessage('editor.document.getWordRangeAtPosition failed');
            return
          }                                    
          
          text = editor.document.getText(range); // get cursor text
        } 
        else {
          text = editor.document.getText(editor.selection); // get selection
        }
          
        expression = `-exec python import sys; sys.path[:0]=["${projectPath}/mem","${extensionPath}"]; import importlib, mem; importlib.reload(mem); from mem import *;`;
       
        try {
          await session.customRequest('evaluate', {
            expression: expression,
            context: 'repl',
            frameId: frameId
          });
        } 
        catch (e) {
          //vscode.window.showErrorMessage('mem expression failed: ' + e.message);
          return;
        }

        expression = `print('${text} =', mem('${text}', None, 12))`;

        expression = JSON.stringify(expression);
        const expr = `-exec python exec(${expression})`;
        try {
          await session.customRequest('evaluate', {
            expression: expr,
            context: 'repl',
            frameId: frameId
          });
          
          vscode.commands.executeCommand('workbench.panel.repl.view.focus');
        } catch (e) {
          //vscode.window.showErrorMessage('mem expr failed: ' + e.message);
          return;
        }
      }
      else{ // c++ program, python code 
        let text;
        if (editor.selection.isEmpty) {
          text = editor.document.getText(); // get full text
        } 
        else {
          text = editor.document.getText(editor.selection); // get selection
        }

        expression = `-exec python import sys; sys.path[:0]=["${filePath}","${extensionPath}"]; import importlib, mem; importlib.reload(mem); from mem import *;`;
        
        try {
          await session.customRequest('evaluate', {
            expression: expression,
            context: 'repl',
            frameId: frameId
          });
        } catch (e) {
          //vscode.window.showErrorMessage('mem expression failed: ' + e.message);
          return;
        } 

        const lines = text.split(/\r?\n/).map(line => line.replace(/\s+$/, ''));  // trim right
        expression = lines.join(' \n');

        expression = JSON.stringify(expression);
        const expr = `-exec python exec(${expression})`;
        try {
          await session.customRequest('evaluate', {
            expression: expr,
            context: 'repl',
            frameId: frameId
          });
        } catch (e) {
          //vscode.window.showErrorMessage('mem expression failed: ' + e.message);
          return;
        } 
      }
    }
    else{ // python program, python code
      if(isPythonFile){ // debug in python code 
        if (editor.selection.isEmpty) { // get cursor text
          const pos = editor.selection.active;               
          const range = editor.document.getWordRangeAtPosition(pos);
          if (!range){
            return
          }                                    
          
          const text = editor.document.getText(range); // get cursor text

          expression = `print('${text} =' , ${text})`;

          try {
            await session.customRequest('evaluate', {
              expression: expression,
              context: 'repl',
              frameId: frameId
            });
            
            vscode.commands.executeCommand('workbench.panel.repl.view.focus');
          } catch (e) {
            //vscode.window.showErrorMessage('mem expression failed: ' + e.message);
            return;
          }          
        } 
        else { // get selection
          text = editor.document.getText(editor.selection); // get selection

          expression = text;

          try {
            await session.customRequest('evaluate', {
              expression: expression,
              context: 'repl',
              frameId: frameId
            });
          } catch (e) {
            //vscode.window.showErrorMessage('mem expression failed: ' + e.message);
            return;
          }             
        }
      }
    }
  });
  context.subscriptions.push(cmd);
}

function deactivate() {}
module.exports = { activate, deactivate };
