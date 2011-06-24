<?php
//http://127.0.0.1:2688/?name=test&url=test/test.php&data=?action%3Dwrite
//echo "\nget data  : " . $_GET['data'].PHP_EOL;
//echo "\npost data : " . $_POST['data'].PHP_EOL;
if($_GET['action'] == 'write'){
	file_put_contents("test.out", "GET : The action is write.\n", FILE_APPEND | LOCK_EX);
}else if($_GET['action'] == 'read'){
	file_put_contents("test.out", "GET : The action is read.\n", FILE_APPEND | LOCK_EX);
}else if($_POST['action'] == 'write'){
	file_put_contents("test.out", "POST : The action is write.\n", FILE_APPEND | LOCK_EX);
}else if($_POST['action'] == 'read'){
	file_put_contents("test.out", "POST : The action is read.\n", FILE_APPEND | LOCK_EX);
}else{
	file_put_contents("test.out", "Undefined action.\n", FILE_APPEND | LOCK_EX);
}
echo "SYNCHTTP_SYNC_SUCCESS";

