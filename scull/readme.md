``` bash
make
```

> 커널 모듈의 타겟 아키텍쳐 없이 로컬 환경에서 진행하기에 `CROSS_COMPILE`을 설정하지 않아도 된다.

``` bash
sudo insmod scull.ko
```

`dmesg | tail -3` 명령어를 통해 커널 로그를 확인하고 생성된 장치의 major 번호를 확인해야 한다.

만약 `class_create` + `device_create` 방식으로 디바이스 노드를 제작하고 `udev`에서 디바이스를 인식하게 해 `sysfs`에 노출시킬 수 있다.

그러나 현재 사용하는 `alloc_chrdev_region`은 문자 디바이스를 위해 메모리 공간을 할당하고 번호를 예약하며 `cdev_add`는 특정 번호에 장치를 연결하는 것 뿐이므로 사용자에게 노출시키기 위해서는 다음과 같은 `mknod`명령어를 사용해야 한다.

여기서 `{device_name}`은 본인 마음대로 수정해도 된다. 그러나 이후 진행될 명령어에서는 모두 동일하게 사용해야 한다.

> 코드 내에서 정의된 `DEVICE_NAME`은 커널에서 관리 목적으로 정의하는 것
>
> `cat /proc/devices`로 확인 가능

<br>

``` bash
sudo mknod /dev/{device_name} c {major} {minor(default:0)}
sudo chmod 666 /dev/{device_name}
```

이후에는 간단히 `echo` + `cat` 조합으로 디바이스에 전달된 데이터를 확인할 수 있다.

예시는 다음과 같다.

``` bash
echo "test" > /dev/{device_name}
cat /dev/{device_name}
```