<?php
include_once("synchttp_client.php");

$host = "127.0.0.1";
$port = "2688";
$url = "http://127.0.0.1:2688/?url=client/recv.php?";
$data = array(
	"action"=>"read",
);

$synchttp = new synchttp();

echo "The get method ";
$res = $synchttp->http_get($host, $port, $url, $data);
if($res == false){
	echo "ERROR";
	echo "<br/>";
}else{
	echo $res;
	echo "<br/>";
}

echo "The post method ";
$res = $synchttp->http_post($host, $port, $url, $data);
if($res == false){
	echo "ERROR";
	echo "<br/>";
}else{
	echo $res;
	echo "<br/>";
}
